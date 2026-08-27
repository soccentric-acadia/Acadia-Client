add_library(acadia_run STATIC
    RunOrchestrator.cc
)

target_include_directories(acadia_run PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${HAL_INCLUDE_DIR}
)

target_link_libraries(acadia_run PRIVATE
    acadia_hal_bridge
    acadia_protos
    spdlog::spdlog
)
