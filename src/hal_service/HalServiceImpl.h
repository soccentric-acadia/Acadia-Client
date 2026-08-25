#pragma once
// hal_service/HalServiceImpl.h — Client-side HalService gRPC server.
//
// This is the "off-the-record" front door that PySDK connects to directly.
// INVARIANTS (from req.md 0.4):
//   - Dispatches ALL test execution through the shared AdapterRegistry (invariant 0.2).
//   - MUST NOT write to any database (invariant 0.3).
//   - MUST NOT call hardware directly — only through AdapterRegistry (invariant 0.1).

#include "registry/AdapterRegistry.h"
#include "registry/ModuleRegistry.h"

#include "hal_service.grpc.pb.h"
#include "common.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace acadia::client::hal_service {

class HalServiceImpl final : public hal::HalService::Service {
public:
    HalServiceImpl(std::shared_ptr<AdapterRegistry> adapter_reg,
                   const ModuleRegistry&             module_reg);

    // ── Unary RPC ─────────────────────────────────────────────────────────
    ::grpc::Status RunSingleShot(
        ::grpc::ServerContext*         ctx,
        const hal::SingleShotRequest*  req,
        hal::common::ResultEnvelope*   resp) override;

    // ── Server-streaming RPCs ─────────────────────────────────────────────
    ::grpc::Status StartLongTerm(
        ::grpc::ServerContext*                              ctx,
        const hal::LongTermRequest*                        req,
        ::grpc::ServerWriter<hal::common::ResultEnvelope>* writer) override;

    ::grpc::Status StartMonitoring(
        ::grpc::ServerContext*                              ctx,
        const hal::MonitoringRequest*                      req,
        ::grpc::ServerWriter<hal::common::ResultEnvelope>* writer) override;

    ::grpc::Status StartEventBased(
        ::grpc::ServerContext*                              ctx,
        const hal::EventBasedRequest*                      req,
        ::grpc::ServerWriter<hal::common::ResultEnvelope>* writer) override;

    // ── Control RPCs ─────────────────────────────────────────────────────
    ::grpc::Status Stop(
        ::grpc::ServerContext*   ctx,
        const hal::StopRequest*  req,
        hal::StopResponse*       resp) override;

    ::grpc::Status Pause(
        ::grpc::ServerContext*    ctx,
        const hal::PauseRequest*  req,
        hal::PauseResponse*       resp) override;

    ::grpc::Status Resume(
        ::grpc::ServerContext*     ctx,
        const hal::ResumeRequest*  req,
        hal::ResumeResponse*       resp) override;

    // ── Discovery RPC ────────────────────────────────────────────────────
    ::grpc::Status ListModules(
        ::grpc::ServerContext*          ctx,
        const hal::ListModulesRequest*  req,
        hal::ListModulesResponse*       resp) override;

private:
    // Validate a module_id against the registry.
    ::grpc::Status validate_module(const std::string& module_id);

    // Build a ResultEnvelope from one hal::TestResult.
    static hal::common::ResultEnvelope make_envelope(
        const std::string&         module_id,
        hal::common::ExecutionMode mode,
        const hal::TestResult&     r);

    // ── Per-session lifecycle ─────────────────────────────────────────────
    // Task 68: sessions are keyed by session_id so concurrent same-module runs
    // are independently controllable. Each start mints a unique id.
    struct Session {
        std::atomic<bool> stop_requested{false};
        std::atomic<bool> paused{false};
        std::string       session_id;   // unique per start
        std::string       module_id;    // module this session runs
    };

    // Mint a new Session registered under a fresh session_id. Returns {id, session}.
    std::pair<std::string, std::shared_ptr<Session>>
    acquire_session(const std::string& module_id);
    void release_session(const std::string& session_id);

    // Look up a session by id (exact) or, when session_id is empty, any session
    // for the module (all-of-module semantics for Stop/Pause/Resume).
    std::shared_ptr<Session> find_session(const std::string& module_id,
                                          const std::string& session_id);

    std::shared_ptr<AdapterRegistry> adapter_reg_;
    const ModuleRegistry&            module_reg_;

    std::mutex                                                  sessions_mu_;
    std::unordered_map<std::string, std::shared_ptr<Session>>   sessions_; // key = session_id
};

} // namespace acadia::client::hal_service
