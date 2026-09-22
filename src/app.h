#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include "config/client_config.h"
#include "spool/Spool.h"
#include "stream/StreamClient.h"
#include "run/RunOrchestrator.h"
#include "log/LogCollector.h"
#include "janitor/Janitor.h"
#include "registry/ModuleRegistry.h"
#include "diag/DiagSocket.h"

// gRPC forward declarations are version-dependent; include the minimal header.
#include <grpcpp/server.h>

namespace acadia::client {

namespace hal_service { class HalServiceImpl; }

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

    // Task 77: durable cursor for cursor-based spool replay. Persisted so a
    // large backlog resumes incrementally instead of always from the oldest row.
    int64_t replay_cursor_ins_seq_{0};  // CLI-080: monotonic cursor

    // HalService gRPC server — always-on inbound endpoint for PySDK.
    // Runs in hal_service_thread_; no DB writes on this path.
    std::unique_ptr<::grpc::Server> hal_service_server_;
    // CLI-009: RegisterService does NOT take ownership — the service must
    // outlive the server, so it is held here explicitly.
    std::unique_ptr<hal_service::HalServiceImpl> hal_service_;
    // CLI-008: credentials for the inbound HalService (TLS/mTLS or plaintext).
    std::shared_ptr<::grpc::ServerCredentials> hal_service_credentials_;
    std::thread                     hal_service_thread_;
};

} // namespace acadia::client

