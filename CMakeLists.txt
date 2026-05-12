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

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------

add_executable(acadia-client
  src/main.cpp
  src/app.cpp
  src/config/client_config.cpp
  src/config/client_id.cpp
)

target_include_directories(acadia-client PRIVATE src)

target_link_libraries(acadia-client PRIVATE
  spdlog::spdlog
  tomlplusplus::tomlplusplus
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
