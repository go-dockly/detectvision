# edge

**Low-latency CV detector pipeline**
video → ONNX YOLO → watchlist alerts → NATS → ClickHouse. 
Built as a systems-level exploration of real-time on-prem recognition flows.

```
┌──────────────┐  grpc   ┌──────────────┐  grpc   ┌──────────────────┐
│ edge_client  │ ──────► │ ingest_server│ ──────► │ nats_publisher   │
│ (CV pipeline)│         │   :50052     │         │     :50051       │
└──────────────┘         └──────────────┘         └────────┬─────────┘
                                                           │ JetStream
                                                           ▼
                                                  ┌─────────────────┐
                                                  │  NATS stream    │
                                                  │  CV_ALERTS      │
                                                  │  subject:       │
                                                  │  cv.alert       │
                                                  └────────┬────────┘
                                       ┌───────────────────┼───────────────┐
                                       ▼                   ▼               
                                  consumer         clickhouse_consumer     
                               (prints alerts)     → cv_detections table   
```

![Demo](assets/demo.png)
![DB](assets/clickhouse.png)

## Prerequisites

- CMake ≥ 3.20, C++20
- OpenCV, ONNX Runtime
- protobuf + grpc
- NATS server

```bash
# macOS
brew install opencv onnxruntime protobuf grpc

# Ubuntu
sudo apt install cmake g++ libopencv-dev \
  libprotobuf-dev protobuf-compiler libgrpc++-dev protobuf-compiler-grpc
```

## Docker client

Messaging stack:

```bash
./assets/download_model.sh

docker compose up -d

# run client on a video file
docker compose run --rm   -v /absolute/path/to/video.mp4:/data/video.mp4:ro   client

# or set VIDEO_FILE as default volume mount
VIDEO_FILE=/absolute/path/to/video.mp4 docker compose run --rm client
```

## Pipeline

1. **Capture** – OpenCV `VideoCapture` (USB index or file) on its own thread, drop-old queue.
2. **Inference** – ONNX YOLO (letterbox + CHW, NMS inside detector).
3. **Post-process** – confidence floor, class filter, in-memory watchlist.
4. **Alert** → grpc `IngestAlert` (frame_id, boxes, confidence, e2e latency, watchlist hit).

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DONNXRUNTIME_ROOT=/path/to/onnxruntime
cmake --build build -j
```

## Stream with grpcurl (or write a tiny client)
```bash
grpcurl -plaintext -d '{
  "source": "sample.mp4",
  "only_with_detections": false,
  "jpeg_quality": 85
}' localhost:50053 detection.v1.AnnotatedVideoService/StreamAnnotatedVideo
```

## ClickHouse

Table created by `clickhouse_consumer`:

```sql
CREATE TABLE IF NOT EXISTS cv_detections (
    frame_id         Int64,
    ts               DateTime64(9, 'UTC'),
    source           String,
    watchlist_hit    UInt8,
    matched_label    String,
    e2e_latency_ms   Float64,
    class_id         Int32,
    class_name       String,
    confidence       Float32,
    x1 Float32, y1 Float32, x2 Float32, y2 Float32,
    nats_seq         UInt64,
    ingested_at      DateTime64(3, 'UTC') DEFAULT now64(3)
) ENGINE = MergeTree()
ORDER BY (source, ts, frame_id)
TTL toDateTime(ts) + INTERVAL 90 DAY
```

One row per detection:

```bash
docker exec -it edge-clickhouse-1 clickhouse-client --password pass \
  -q "SELECT class_name, count(), avg(confidence), avg(e2e_latency_ms)
      FROM cv_detections GROUP BY class_name ORDER BY count() DESC"
```

Disable: `-DBUILD_CLICKHOUSE_CONSUMER=OFF`.

## Observability (OpenTelemetry structured logging)

Services use **spdlog** to enable at build time

```bash
cmake -B build -DWITH_OTEL=ON \
```
When `WITH_OTEL=OFF` otel code is compiled out. Each process sets `service.name` on init.
