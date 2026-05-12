#include "app.h"

#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>

namespace acadia::client {

App::App(config::ClientConfig cfg) : cfg_(std::move(cfg)) {}
App::~App() = default;

bool App::init() {
    spdlog::info("app init: client_id={} server={}:{}",
        cfg_.client_id, cfg_.server_address, cfg_.server_port);

    // Subsystems are initialised here as they land:
    //   Step 2-5: spool (result_store, log_store, janitor, writer thread)
    //   Step 6-7: gRPC stream client
    //   Step 8:   run registry + state machine
    //   Step 9+:  HAL drivers

    return true;
}

void App::run(std::atomic<bool>& shutdown) {
    spdlog::info("app running — waiting for SIGTERM/SIGINT");

    while (!shutdown.load(std::memory_order_relaxed)) {
        // Event loop will live here once gRPC stream is wired in.
        // For now, yield the CPU rather than spin.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    spdlog::info("shutdown signal received — stopping subsystems");
    // Subsystem teardown in reverse-init order goes here.
}

} // namespace acadia::client
