cmake_minimum_required(VERSION 3.20)
project(acadia-client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerWarnings.cmake)
include(FetchContent)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# spdlog
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(spdlog)
endif()

# toml++ (config file parsing)
find_package(tomlplusplus QUIET)
if(NOT tomlplusplus_FOUND)
  FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(tomlplusplus)
endif()

# yaml-cpp (modules.yaml registry)
find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
  FetchContent_Declare(
    yaml_cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
    GIT_SHALLOW    TRUE
  )
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(yaml_cpp)
endif()

# SQLite3
find_package(SQLite3 REQUIRED)

# gRPC + Protobuf (system preferred; cross-compile from Yocto SDK)
find_package(gRPC REQUIRED)
find_package(Protobuf REQUIRED)

# ---------------------------------------------------------------------------
# Proto generation
# ---------------------------------------------------------------------------

set(PROTO_DIR "${CMAKE_SOURCE_DIR}/../Acadia-HAL/protos")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

# Generated sources
set(PROTO_SRCS
    ${PROTO_GEN_DIR}/common.pb.cc
    ${PROTO_GEN_DIR}/hal_service.pb.cc
    ${PROTO_GEN_DIR}/hal_service.grpc.pb.cc
    ${PROTO_GEN_DIR}/client_service.pb.cc
    ${PROTO_GEN_DIR}/client_service.grpc.pb.cc
)

add_custom_command(
    OUTPUT ${PROTO_SRCS}
    COMMAND protoc
        --proto_path=${PROTO_DIR}
        --cpp_out=${PROTO_GEN_DIR}
        --grpc_out=${PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        ${PROTO_DIR}/common.proto
        ${PROTO_DIR}/hal_service.proto
        ${PROTO_DIR}/client_service.proto
    DEPENDS
        ${PROTO_DIR}/common.proto
        ${PROTO_DIR}/hal_service.proto
        ${PROTO_DIR}/client_service.proto
    VERBATIM
)

add_library(acadia_protos STATIC ${PROTO_SRCS})
target_include_directories(acadia_protos PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(acadia_protos PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------

add_executable(acadia-client
  src/main.cpp
  src/app.cpp
  src/config/client_config.cpp
  src/config/client_id.cpp
  src/spool/Spool.cc
  src/stream/StreamClient.cc
  src/registry/ModuleRegistry.cc
  src/run/RunOrchestrator.cc
  src/log/LogCollector.cc
  src/janitor/Janitor.cc
  src/diag/DiagSocket.cc
  src/ota/OtaClient.cc
)

target_include_directories(acadia-client PRIVATE src ${PROTO_GEN_DIR})

target_link_libraries(acadia-client PRIVATE
  acadia_protos
  spdlog::spdlog
  tomlplusplus::tomlplusplus
  yaml-cpp::yaml-cpp
  SQLite::SQLite3
  gRPC::grpc++
  protobuf::libprotobuf
)

target_compile_definitions(acadia-client PRIVATE
  ACADIA_CLIENT_VERSION="${PROJECT_VERSION}"
)


set_compiler_warnings(acadia-client)

# ---------------------------------------------------------------------------
# Optional sanitizers
# ---------------------------------------------------------------------------

option(ENABLE_SANITIZERS "Enable -fsanitize=address,undefined" OFF)
if(ENABLE_SANITIZERS)
  target_compile_options(acadia-client PRIVATE -fsanitize=address,undefined)
  target_link_options(acadia-client PRIVATE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

install(TARGETS acadia-client RUNTIME DESTINATION bin)
install(FILES conf/client.conf.sample
  DESTINATION /etc/acadia
  RENAME client.conf
  OPTIONAL
)
install(FILES systemd/acadia-client.service
  DESTINATION /lib/systemd/system
  OPTIONAL
)
