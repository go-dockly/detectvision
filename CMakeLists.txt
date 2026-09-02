cmake_minimum_required(VERSION 3.20)
project(edge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# System deps
find_package(Threads REQUIRED)
find_package(OpenCV QUIET COMPONENTS core imgproc imgcodecs videoio highgui)
if(NOT OpenCV_FOUND)
  message(STATUS "OpenCV not found – edge_cv / edge_client will be skipped")
endif()

# Protobuf + gRPC
# Homebrew often ships a protobuf CMake export set that is incomplete relative
# to what gRPC's CONFIG package expects (missing libupb targets). Prefer
# pkg-config on Apple; fall back to CONFIG elsewhere, then pkg-config.
find_package(PkgConfig REQUIRED)

set(_use_grpc_config FALSE)
if(NOT APPLE)
  # CONFIG path: let gRPC pull Protobuf as a dependency
  find_package(gRPC CONFIG QUIET)
  if(gRPC_FOUND)
    set(_use_grpc_config TRUE)
  endif()
endif()

if(_use_grpc_config)
  set(_GRPC_GRPCPP gRPC::grpc++)
  if(TARGET gRPC::grpc_cpp_plugin)
    set(_GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
  else()
    find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  endif()
  # Protobuf targets come from gRPC's find_dependency
  if(NOT TARGET protobuf::libprotobuf)
    find_package(Protobuf REQUIRED)
  endif()
  message(STATUS "Using gRPC (CONFIG)")
else()
  # pkg-config path (reliable on Homebrew / macOS)
  pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
  pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)
  # Provide protobuf::libprotobuf alias if missing so rest of file is uniform
  if(NOT TARGET protobuf::libprotobuf)
    add_library(protobuf::libprotobuf ALIAS PkgConfig::PROTOBUF)
  endif()
  # well-known types (timestamp.proto etc.) live under the protobuf include dir
  if(PROTOBUF_INCLUDE_DIRS)
    set(Protobuf_INCLUDE_DIRS ${PROTOBUF_INCLUDE_DIRS})
  else()
    # Homebrew fallback
    execute_process(COMMAND brew --prefix protobuf
      OUTPUT_VARIABLE _PB_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(_PB_PREFIX)
      set(Protobuf_INCLUDE_DIRS "${_PB_PREFIX}/include")
    endif()
  endif()
  set(_GRPC_GRPCPP PkgConfig::GRPC)
  find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
  find_program(_PROTOBUF_PROTOC protoc REQUIRED)
  message(STATUS "Using gRPC + Protobuf via pkg-config")
  message(STATUS "  Protobuf includes: ${Protobuf_INCLUDE_DIRS}")
endif()

if(NOT DEFINED _PROTOBUF_PROTOC)
  if(TARGET protobuf::protoc)
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
  else()
    find_program(_PROTOBUF_PROTOC protoc REQUIRED)
  endif()
endif()

# ONNX Runtime
if(NOT DEFINED ONNXRUNTIME_ROOT)
  if(DEFINED ENV{ONNXRUNTIME_ROOT})
    set(ONNXRUNTIME_ROOT $ENV{ONNXRUNTIME_ROOT})
  endif()
endif()
if(ONNXRUNTIME_ROOT)
  set(ONNXRUNTIME_INCLUDE_DIRS ${ONNXRUNTIME_ROOT}/include)
  find_library(ONNXRUNTIME_LIB onnxruntime
    PATHS ${ONNXRUNTIME_ROOT}/lib ${ONNXRUNTIME_ROOT}/lib64
    NO_DEFAULT_PATH)
else()
  find_path(ONNXRUNTIME_INCLUDE_DIRS onnxruntime_cxx_api.h)
  find_library(ONNXRUNTIME_LIB onnxruntime)
endif()
if(NOT ONNXRUNTIME_LIB)
  message(WARNING "ONNX Runtime not found – edge_cv client will not build")
endif()

option(WITH_OTEL "Enable OpenTelemetry structured logging via spdlog sink" OFF)

if(WITH_OTEL)
  message(STATUS "OpenTelemetry support: ON")

  # Core SDK (Logs + Resource + OTLP HTTP exporter)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(WITH_OTLP_HTTP ON CACHE BOOL "" FORCE)
  set(WITH_OTLP_GRPC OFF CACHE BOOL "" FORCE)   # keep deps light; HTTP enough
  set(WITH_LOGS_PREVIEW ON CACHE BOOL "" FORCE) # older tags; no-op on recent
  set(WITH_STL ON CACHE BOOL "" FORCE)

  FetchContent_Declare(opentelemetry-cpp
    GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(opentelemetry-cpp)

  # spdlog sink lives in the contrib repo
  FetchContent_Declare(opentelemetry-cpp-contrib
    GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp-contrib.git
    GIT_TAG main
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(opentelemetry-cpp-contrib)
  if(NOT opentelemetry-cpp-contrib_POPULATED)
    FetchContent_Populate(opentelemetry-cpp-contrib)
  endif()
  add_subdirectory(
    ${opentelemetry-cpp-contrib_SOURCE_DIR}/instrumentation/spdlog
    ${CMAKE_BINARY_DIR}/otel-spdlog-sink
  )
else()
  message(STATUS "OpenTelemetry support: OFF (enable with -DWITH_OTEL=ON)")
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(json)
endif()

find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1 GIT_SHALLOW TRUE)
  set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
  set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(spdlog)
endif()

# NATS C client
find_package(NATS QUIET)
if(NOT NATS_FOUND)
  FetchContent_Declare(nats
    GIT_REPOSITORY https://github.com/nats-io/nats.c.git
    GIT_TAG v3.9.1 GIT_SHALLOW TRUE)
  set(NATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_STREAMING OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_LIB_SHARED OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_LIB_STATIC ON CACHE BOOL "" FORCE)
  set(NATS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(nats)
  set(NATS_LIBRARIES nats_static)
  set(NATS_INCLUDE_DIRS ${nats_SOURCE_DIR}/src)
endif()

# Protobuf / gRPC
set(PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_FILES
  ${PROTO_ROOT}/common/v1/error.proto
  ${PROTO_ROOT}/detection/v1/detection.proto
  ${PROTO_ROOT}/detection/v1/ingest_service.proto
  ${PROTO_ROOT}/nats/v1/publisher_service.proto
)

set(PROTO_SRCS)
set(PROTO_HDRS)
set(GRPC_SRCS)
set(GRPC_HDRS)

foreach(proto ${PROTO_FILES})
  get_filename_component(proto_name ${proto} NAME_WE)
  get_filename_component(proto_dir ${proto} DIRECTORY)
  file(RELATIVE_PATH rel_dir ${PROTO_ROOT} ${proto_dir})

  set(pb_cc  ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.pb.cc)
  set(pb_h   ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.pb.h)
  set(grpc_cc ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.grpc.pb.cc)
  set(grpc_h  ${PROTO_GEN_DIR}/${rel_dir}/${proto_name}.grpc.pb.h)

  add_custom_command(
    OUTPUT ${pb_cc} ${pb_h} ${grpc_cc} ${grpc_h}
    COMMAND ${_PROTOBUF_PROTOC}
      --proto_path=${PROTO_ROOT}
      --proto_path=${Protobuf_INCLUDE_DIRS}
      --cpp_out=${PROTO_GEN_DIR}
      --grpc_out=${PROTO_GEN_DIR}
      --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
      ${proto}
    DEPENDS ${proto}
    COMMENT "Generating ${rel_dir}/${proto_name}"
  )
  list(APPEND PROTO_SRCS ${pb_cc})
  list(APPEND PROTO_HDRS ${pb_h})
  list(APPEND GRPC_SRCS ${grpc_cc})
  list(APPEND GRPC_HDRS ${grpc_h})
endforeach()

add_library(edge_proto ${PROTO_SRCS} ${GRPC_SRCS})
target_include_directories(edge_proto PUBLIC
  ${PROTO_GEN_DIR}
  ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(edge_proto PUBLIC
  protobuf::libprotobuf
  ${_GRPC_GRPCPP}
)

# common
add_library(edge_common
  src/common/otel.cpp
)
target_include_directories(edge_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${NATS_INCLUDE_DIRS}
)
target_link_libraries(edge_common PUBLIC
  edge_proto
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  ${NATS_LIBRARIES}
  Threads::Threads
)

if(WITH_OTEL)
  target_compile_definitions(edge_common PUBLIC WITH_OTEL)
  target_link_libraries(edge_common PUBLIC
    opentelemetry_logs
    opentelemetry_exporter_otlp_http
    opentelemetry_resources
    opentelemetry_common
    opentelemetry_spdlog_sink   # from contrib
  )
  target_include_directories(edge_common PUBLIC
    ${opentelemetry-cpp-contrib_SOURCE_DIR}/instrumentation/spdlog/include
  )
endif()

# CV pipeline lib + client (OpenCV / ONNX)
if(OpenCV_FOUND AND ONNXRUNTIME_LIB)
  add_library(edge_cv
    src/cv/src/capture.cpp
    src/cv/src/detector.cpp
    src/cv/src/pipeline.cpp
    src/cv/src/postprocess.cpp
  )
  target_include_directories(edge_cv PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cv/include
    ${OpenCV_INCLUDE_DIRS}
    ${ONNXRUNTIME_INCLUDE_DIRS}
  )
  target_link_libraries(edge_cv PUBLIC
    ${OpenCV_LIBS}
    ${ONNXRUNTIME_LIB}
    Threads::Threads
  )

  add_executable(edge_client src/client/main.cpp)
  target_link_libraries(edge_client PRIVATE
    edge_cv edge_common edge_proto
  )
endif()

add_executable(nats_publisher
  src/nats_publisher/main.cpp
  src/nats_publisher/publisher_service.cpp
)
target_link_libraries(nats_publisher PRIVATE edge_common)

add_executable(ingest_server
  src/ingest/main.cpp
  src/ingest/ingest_service.cpp
)
target_link_libraries(ingest_server PRIVATE edge_common)

add_executable(consumer src/consumer/main.cpp)
target_link_libraries(consumer PRIVATE edge_common)

# ClickHouse consumer
option(BUILD_CLICKHOUSE_CONSUMER "Build clickhouse_consumer binary" ON)
if(BUILD_CLICKHOUSE_CONSUMER)
  set(WITH_OPENSSL ON CACHE BOOL "" FORCE)
  set(CH_USE_ABSEIL_FOR_BIGNUM OFF CACHE BOOL "" FORCE)
  set(CH_MAP_BOOL_TO_UINT8 ON CACHE BOOL "" FORCE)
  FetchContent_Declare(clickhouse-cpp
    GIT_REPOSITORY https://github.com/ClickHouse/clickhouse-cpp.git
    GIT_TAG v2.6.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(clickhouse-cpp)

  add_executable(clickhouse_consumer src/clickhouse_consumer/main.cpp)
  target_link_libraries(clickhouse_consumer PRIVATE
    edge_common
    clickhouse-cpp-lib
  )
  message(STATUS "  clickhouse_consumer: ON")
else()
  message(STATUS "  clickhouse_consumer: OFF")
endif()

message(STATUS "edge configured (C++${CMAKE_CXX_STANDARD})")
message(STATUS "  OpenCV: ${OpenCV_VERSION}")
message(STATUS "  ONNX Runtime: ${ONNXRUNTIME_LIB}")
