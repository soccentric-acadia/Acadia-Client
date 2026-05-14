#pragma once

#include <atomic>
#include <memory>
#include "config/client_config.h"
#include "spool/Spool.h"
#include "stream/StreamClient.h"
#include "run/RunOrchestrator.h"
#include "log/LogCollector.h"
#include "janitor/Janitor.h"
#include "registry/ModuleRegistry.h"
#include "diag/DiagSocket.h"

namespace acadia::client {

// Top-level application object. Owns all subsystems.
// Constructed in main, never copied.
class App {
public:
    explicit App(config::ClientConfig cfg);
    ~App();

    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    // One-time initialisation. Returns false on fatal error.
    bool init();

    // Blocks until shutdown is set to true.
    void run(std::atomic<bool>& shutdown);

private:
    void on_server_message(const acadia::stream::ServerMessage& msg);
    void send_hello();
    void send_unacked_replay();

    config::ClientConfig       cfg_;

    std::unique_ptr<Spool>           spool_;
    std::unique_ptr<StreamClient>    stream_;
    std::unique_ptr<ModuleRegistry>  registry_;
    std::unique_ptr<RunOrchestrator> orchestrator_;
    std::unique_ptr<LogCollector>    log_collector_;
    std::unique_ptr<Janitor>         janitor_;
    std::unique_ptr<DiagSocket>      diag_socket_;
};

} // namespace acadia::client

