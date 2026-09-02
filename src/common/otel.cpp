#include "common/otel.hpp"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#ifdef WITH_OTEL

#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter_options.h"
#include "opentelemetry/logs/provider.h"
#include "opentelemetry/sdk/logs/logger_provider_factory.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor_factory.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/common/global_log_handler.h"

// Official contrib sink
#include "opentelemetry/instrumentation/spdlog/sink.h"

namespace nostd     = opentelemetry::nostd;
namespace otlp      = opentelemetry::exporter::otlp;
namespace logs_sdk  = opentelemetry::sdk::logs;
namespace logs_api  = opentelemetry::logs;
namespace resource  = opentelemetry::sdk::resource;

#endif  // WITH_OTEL

namespace edge::otel {
namespace {

std::once_flag g_init_flag;
#ifdef WITH_OTEL
std::shared_ptr<logs_sdk::LoggerProvider> g_provider;
#endif

std::string env_or(const char* key, const std::string& fallback) {
  if (const char* v = std::getenv(key); v && *v) return std::string{v};
  return fallback;
}

}  // namespace

void init(const std::string& service_name,
          const std::string& service_version) {
#ifdef WITH_OTEL
  std::call_once(g_init_flag, [&] {
    // ----- Resource -------------------------------------------------
    auto res = resource::Resource::Create({
        {"service.name", service_name},
        {"service.version", service_version},
        {"deployment.environment",
         env_or("OTEL_ENVIRONMENT", "development")},
    });

    // ----- Exporter (OTLP/HTTP – easiest for local Collector) -------
    otlp::OtlpHttpLogRecordExporterOptions opts;
    opts.url = env_or("OTEL_EXPORTER_OTLP_ENDPOINT",
                      "http://localhost:4318/v1/logs");
    // Honour the classic OTEL_EXPORTER_OTLP_LOGS_ENDPOINT if set
    if (const char* logs_ep = std::getenv("OTEL_EXPORTER_OTLP_LOGS_ENDPOINT");
        logs_ep && *logs_ep) {
      opts.url = logs_ep;
    }

    auto exporter =
        otlp::OtlpHttpLogRecordExporterFactory::Create(opts);

    // ----- Processor (batch for production)
    auto processor =
        logs_sdk::BatchLogRecordProcessorFactory::Create(std::move(exporter));

    g_provider = logs_sdk::LoggerProviderFactory::Create(
        std::move(processor), res);

    // Make it the global provider so the spdlog sink can find it
    nostd::shared_ptr<logs_api::LoggerProvider> api_provider(g_provider);
    logs_api::Provider::SetLoggerProvider(api_provider);

    // ----- Install OTel sink alongside the existing coloured console
    auto otel_sink =
        std::make_shared<spdlog::sinks::opentelemetry_sink_mt>();
    otel_sink->set_level(spdlog::level::info);

    // Keep a colour console sink so local `docker compose logs` still work
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_level(spdlog::level::info);

    auto logger = std::make_shared<spdlog::logger>(
        "edge", spdlog::sinks_init_list{console, otel_sink});
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);

    spdlog::info("OpenTelemetry logging initialised  service={}  endpoint={}",
                 service_name, opts.url);
  });
#else
  (void)service_name;
  (void)service_version;
  // No-op when OTel is not compiled in – existing spdlog setup remains.
#endif
}

void shutdown() {
#ifdef WITH_OTEL
  if (g_provider) {
    g_provider->ForceFlush();
    nostd::shared_ptr<logs_api::LoggerProvider> none;
    logs_api::Provider::SetLoggerProvider(none);
    g_provider.reset();
  }
#endif
  spdlog::shutdown();
}

}