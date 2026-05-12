#pragma once

#include <atomic>
#include "config/client_config.h"

namespace acadia::client {

// Top-level application object. Owns all subsystems (spool, gRPC stream,
// run registry, log collector, janitor). Constructed in main, never copied.
// Each subsystem will be added here as it is implemented per the build order.
class App {
public:
    explicit App(config::ClientConfig cfg);
    ~App();

    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    // One-time initialisation. Returns false on fatal error; caller should exit(1).
    bool init();

    // Blocks until shutdown is set to true (via signal handler).
    void run(std::atomic<bool>& shutdown);

private:
    config::ClientConfig cfg_;
};

} // namespace acadia::client
