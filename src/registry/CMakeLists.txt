add_library(acadia_registry STATIC
    ModuleRegistry.cc
    AdapterRegistry.cc
    adapters/cpu_adapter.cc
    adapters/thermal_adapter.cc
    adapters/system_memory_adapter.cc
    adapters/stub_adapter.cc
)

target_include_directories(acadia_registry PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/../Acadia-HAL/src
)

target_link_libraries(acadia_registry PRIVATE
    acadia_protos
    yaml-cpp::yaml-cpp
    spdlog::spdlog
)
