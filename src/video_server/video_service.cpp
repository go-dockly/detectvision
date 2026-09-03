#include "video_service.hpp"

#include <filesystem>
#include <sstream>

#include "absl/base/config.h"
#include "absl/base/options.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <spdlog/spdlog.h>

#include <clickhouse/columns/numeric.h>
#include <clickhouse/columns/string.h>

namespace fs = std::filesystem;

namespace edge {

AnnotatedVideoServiceImpl::AnnotatedVideoServiceImpl(
    std::unique_ptr<clickhouse::Client> ch, std::string video_root)
    : ch_(std::move(ch)), video_root_(std::move(video_root)) {}

std::unordered_map<int64_t, std::vector<DetBox>>
AnnotatedVideoServiceImpl::load_detections(const std::string& source,
                                           int64_t start_frame,
                                           int64_t end_frame) {
  std::unordered_map<int64_t, std::vector<DetBox>> out;

  std::ostringstream sql;
  sql << "SELECT frame_id, class_id, class_name, confidence, x1, y1, x2, y2 "
      << "FROM cv_detections "
      << "WHERE source = '" << source << "' "
      << "AND frame_id >= " << start_frame << " ";
  if (end_frame > 0) {
    sql << "AND frame_id < " << end_frame << " ";
  }
  sql << "ORDER BY frame_id";

  try {
    ch_->Select(sql.str(), [&](const clickhouse::Block& block) {
      if (block.GetColumnCount() == 0 || block.GetRowCount() == 0) {
        return;   // ignore empty progress block
      }
      auto col_fid  = block[0]->As<clickhouse::ColumnInt64>();
      auto col_cid  = block[1]->As<clickhouse::ColumnInt32>();
      auto col_name = block[2]->As<clickhouse::ColumnString>();
      auto col_conf = block[3]->As<clickhouse::ColumnFloat32>();
      auto col_x1   = block[4]->As<clickhouse::ColumnFloat32>();
      auto col_y1   = block[5]->As<clickhouse::ColumnFloat32>();
      auto col_x2   = block[6]->As<clickhouse::ColumnFloat32>();
      auto col_y2   = block[7]->As<clickhouse::ColumnFloat32>();

      for (size_t i = 0; i < block.GetRowCount(); ++i) {
        DetBox b;
        b.class_id   = col_cid->At(i);
        b.class_name = std::string(col_name->At(i));
        b.confidence = col_conf->At(i);
        b.x1 = col_x1->At(i);
        b.y1 = col_y1->At(i);
        b.x2 = col_x2->At(i);
        b.y2 = col_y2->At(i);

        out[col_fid->At(i)].push_back(std::move(b));
      }
    });
  } catch (const std::exception& e) {
    spdlog::error("ClickHouse query failed: {}", e.what());
    throw;
  }

  spdlog::info("loaded detections for {} frames (source={})", out.size(),
               source);
  return out;
}

void AnnotatedVideoServiceImpl::draw_boxes(cv::Mat& frame,
                                           const std::vector<DetBox>& boxes) {
  for (const auto& b : boxes) {
    const cv::Scalar color(0, 255, 0);  // green
    cv::rectangle(frame,
                  cv::Point(static_cast<int>(b.x1), static_cast<int>(b.y1)),
                  cv::Point(static_cast<int>(b.x2), static_cast<int>(b.y2)),
                  color, 2);

    std::ostringstream label;
    label << b.class_name << " " << std::fixed << std::setprecision(2)
          << b.confidence;

    int baseline = 0;
    cv::Size text_size = cv::getTextSize(label.str(), cv::FONT_HERSHEY_SIMPLEX,
                                         0.5, 1, &baseline);

    cv::rectangle(frame,
                  cv::Point(static_cast<int>(b.x1),
                            static_cast<int>(b.y1) - text_size.height - 4),
                  cv::Point(static_cast<int>(b.x1) + text_size.width,
                            static_cast<int>(b.y1)),
                  color, cv::FILLED);

    cv::putText(frame, label.str(),
                cv::Point(static_cast<int>(b.x1), static_cast<int>(b.y1) - 2),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 1);
  }
}

grpc::Status AnnotatedVideoServiceImpl::StreamAnnotatedVideo(
    grpc::ServerContext* context,
    const detection::v1::StreamAnnotatedVideoRequest* request,
    grpc::ServerWriter<detection::v1::AnnotatedFrame>* writer) {

  if (request->source().empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "source is required");
  }

  // Resolve path safely under VIDEO_ROOT
  fs::path video_path;
  if (fs::path(request->source()).is_absolute()) {
    video_path = request->source();
  } else {
    video_path = fs::path(video_root_) / request->source();
  }
  video_path = fs::weakly_canonical(video_path);

  // Basic path traversal protection
  if (!video_path.string().starts_with(fs::weakly_canonical(video_root_).string())) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "source path escapes VIDEO_ROOT");
  }

  if (!fs::exists(video_path)) {
    return grpc::Status(grpc::StatusCode::NOT_FOUND,
                        "video not found: " + video_path.string());
  }

  const int64_t start_frame = std::max<int64_t>(0, request->start_frame_id());
  const int64_t end_frame   = request->end_frame_id();
  const int quality = request->jpeg_quality() > 0 && request->jpeg_quality() <= 100
                          ? request->jpeg_quality()
                          : 85;

  // Pre-load detections (fine for sample.mp4)
  std::unordered_map<int64_t, std::vector<DetBox>> dets_by_frame;
  try {
    // Note: we query using the *original* source string that was stored in CH.
    // Callers must pass the same string the edge_client used (or normalize it).
    dets_by_frame = load_detections(request->source(), start_frame, end_frame);
  } catch (const std::exception& e) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        std::string("ClickHouse error: ") + e.what());
  }

  cv::VideoCapture cap(video_path.string());
  if (!cap.isOpened()) {
    return grpc::Status(grpc::StatusCode::INTERNAL,
                        "failed to open video: " + video_path.string());
  }

  spdlog::info("streaming annotated video {} (start={}, end={}, only_dets={})",
               video_path.string(), start_frame, end_frame,
               request->only_with_detections());

  int64_t frame_id = 0;
  cv::Mat frame;
  size_t emitted = 0;

  while (cap.read(frame)) {
    if (context->IsCancelled()) {
      spdlog::info("client cancelled stream after {} frames", emitted);
      break;
    }

    if (frame_id < start_frame) {
      ++frame_id;
      continue;
    }
    if (end_frame > 0 && frame_id >= end_frame) {
      break;
    }

    auto it = dets_by_frame.find(frame_id);
    const bool has_dets = it != dets_by_frame.end() && !it->second.empty();

    if (request->only_with_detections() && !has_dets) {
      ++frame_id;
      continue;
    }

    // Draw
    if (has_dets) {
      draw_boxes(frame, it->second);
    }

    // Encode
    std::vector<uchar> buf;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality};
    if (!cv::imencode(".jpg", frame, buf, params)) {
      spdlog::warn("JPEG encode failed for frame {}", frame_id);
      ++frame_id;
      continue;
    }

    detection::v1::AnnotatedFrame out;
    out.set_frame_id(frame_id);
    out.set_width(frame.cols);
    out.set_height(frame.rows);
    out.set_jpeg(buf.data(), buf.size());
    out.set_detection_count(has_dets ? static_cast<int32_t>(it->second.size()) : 0);

    if (has_dets) {
      for (const auto& b : it->second) {
        auto* d = out.add_detections();
        d->set_class_id(b.class_id);
        d->set_class_name(b.class_name);
        d->set_confidence(b.confidence);
        auto* box = d->mutable_box();
        box->set_x1(b.x1);
        box->set_y1(b.y1);
        box->set_x2(b.x2);
        box->set_y2(b.y2);
      }
    }

    if (!writer->Write(out)) {
      spdlog::info("writer closed by client after {} frames", emitted);
      break;
    }

    ++emitted;
    ++frame_id;
  }

  spdlog::info("finished stream emitted {} annotated frames", emitted);
  return grpc::Status::OK;
}

}