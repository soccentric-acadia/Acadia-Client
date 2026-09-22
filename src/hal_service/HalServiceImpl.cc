// hal_service/HalServiceImpl.cc — Client-side HalService implementation.
//
// All test execution flows through AdapterRegistry → HAL API → HAL lib.
// NO database writes on this path.

#include "hal_service/HalServiceImpl.h"
#include "hal_bridge/ResultSerializer.h"

#include <google/protobuf/any.pb.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace acadia::client::hal_service {

namespace {
// Default cycle interval when the caller doesn't specify one.
constexpr int kDefaultIntervalMs   = 1000;
constexpr int kDefaultPollMs       = 200;
constexpr int kPauseCheckMs        = 50;   // sleep granularity while paused
} // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

HalServiceImpl::HalServiceImpl(std::shared_ptr<AdapterRegistry> adapter_reg,
                                const ModuleRegistry&             module_reg,
                                const std::string&                auth_token)
    : adapter_reg_(std::move(adapter_reg)), module_reg_(module_reg), auth_token_(auth_token) {}

// Task 70 + CLI-102/103: authentication is REQUIRED. An empty configured token
// only permits loopback peers (fail closed for any network peer); the token
// itself is compared in constant time to avoid a timing side channel.
namespace {
bool is_loopback_peer(const std::string& peer) {
    // Peer strings look like "ipv6:[::1]:50052" / "ipv4:127.0.0.1:50052".
    return peer.find("127.0.0.1") != std::string::npos ||
           peer.find("[::1]") != std::string::npos ||
           peer.find("localhost") != std::string::npos ||
           peer.find("unix:") == 0;
}
bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}
} // namespace

::grpc::Status HalServiceImpl::require_auth(::grpc::ServerContext* ctx,
                                             const std::string&     expected_token) {
    if (expected_token.empty()) {
        // CLI-102: the shipped default is an empty token — any NON-loopback
        // peer must be rejected (fail closed); loopback stays open so the
        // local SDK keeps working on a bench device.
        if (ctx != nullptr) {
            const std::string peer = ctx->peer();
            if (!is_loopback_peer(peer)) {
                spdlog::warn("hal_service: unauthenticated non-loopback peer '{}' rejected "
                             "(hal_service_auth_token not configured)", peer);
                return ::grpc::Status(::grpc::StatusCode::UNAUTHENTICATED,
                                      "hal_service_auth_token must be configured for network access");
            }
            return ::grpc::Status::OK;
        }
        return ::grpc::Status::OK;
    }
    if (ctx == nullptr) return ::grpc::Status(::grpc::StatusCode::UNAUTHENTICATED,
                                              "authentication required");
    auto it = ctx->client_metadata().find("authorization");
    if (it == ctx->client_metadata().end())
        return ::grpc::Status(::grpc::StatusCode::UNAUTHENTICATED,
                              "missing authorization header");
    const std::string header(it->second.data(), it->second.size());
    const std::string expected = "Bearer " + expected_token;
    // CLI-103: constant-time comparison.
    if (!constant_time_equal(header, expected))
        return ::grpc::Status(::grpc::StatusCode::UNAUTHENTICATED,
                              "invalid authorization token");
    // CLI-120: audit trail of which peer ran what.
    spdlog::info("hal_service: authorized peer={} ", ctx->peer());
    return ::grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Task 67: forward the populated module_config (google.protobuf.Any) to
// adapters instead of an empty {} payload. Serialize the Any bytes verbatim so
// the adapter can unpack the real module Config.
static std::vector<uint8_t> module_config_bytes(const google::protobuf::Any& any) {
    std::string s;
    if (any.IsInitialized() && any.SerializeToString(&s)) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }
    return {};  // unset Any -> empty payload (adapter default)
}

::grpc::Status HalServiceImpl::validate_module(const std::string& module_id) {
    if (module_id.empty())
        return {::grpc::StatusCode::INVALID_ARGUMENT, "module_id is required"};
    // Check adapter registry — if we have an adapter it's a known module.
    if (!adapter_reg_->has_adapter(module_id))
        return {::grpc::StatusCode::INVALID_ARGUMENT,
                "unknown module_id: " + module_id};
    // CLI-104: stub-backed modules are honestly UNIMPLEMENTED (they used to
    // run the stub adapter and report a SKIPPED verdict as if it were real).
    if (adapter_reg_->is_stub_adapter(module_id))
        return ::grpc::Status(
            ::grpc::StatusCode::UNIMPLEMENTED,
            "module '" + module_id + "' has no HAL implementation (stub only)");
    return ::grpc::Status::OK;
}

// static
hal::common::ResultEnvelope HalServiceImpl::make_envelope(
    const std::string&         module_id,
    hal::common::ExecutionMode mode,
    const hal::TestResult&     r)
{
    namespace br = acadia::client::hal_bridge;

    hal::common::ResultEnvelope env;
    env.set_module_id(module_id);
    env.set_mode(mode);
    env.set_status(br::to_proto(r.output.status));
    if (!r.output.error_message.empty())
        env.set_error_message(r.output.error_message);
    *env.mutable_timestamp() = br::to_proto(r.timestamp);

    // Pack the detailed TestResult into the Any payload.
    env.mutable_payload()->PackFrom(br::to_proto(r));
    return env;
}

std::pair<std::string, std::shared_ptr<HalServiceImpl::Session>>
HalServiceImpl::acquire_session(const std::string& module_id) {
    auto s = std::make_shared<Session>();
    // Task 68: unique session id per start so concurrent same-module runs stay
    // independent (previously one session per module was stomped).
    static std::atomic<uint64_t> counter{0};
    s->session_id = "sess-" + module_id + "-" + std::to_string(counter.fetch_add(1));
    s->module_id = module_id;
    std::lock_guard<std::mutex> lk(sessions_mu_);
    sessions_[s->session_id] = s;
    return {s->session_id, s};
}

void HalServiceImpl::release_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(sessions_mu_);
    sessions_.erase(session_id);
}

// CLI-110: Stop/Pause/Resume with an EMPTY session_id acts on ALL sessions of
// the module (previously an arbitrary one, chosen by hash order).
std::vector<std::shared_ptr<HalServiceImpl::Session>>
HalServiceImpl::find_sessions(const std::string& module_id,
                               const std::string& session_id) {
    std::lock_guard<std::mutex> lk(sessions_mu_);
    std::vector<std::shared_ptr<Session>> out;
    if (!session_id.empty()) {
        auto it = sessions_.find(session_id);
        if (it != sessions_.end() && it->second->module_id == module_id)
            out.push_back(it->second);
        return out;
    }
    for (const auto& kv : sessions_) {
        if (kv.second->module_id == module_id) out.push_back(kv.second);
    }
    return out;
}

// ---------------------------------------------------------------------------
// RunSingleShot  (unary)
// ---------------------------------------------------------------------------

::grpc::Status HalServiceImpl::RunSingleShot(
    ::grpc::ServerContext*        ctx,
    const hal::SingleShotRequest*  req,
    hal::common::ResultEnvelope*   resp)
{
    if (auto st = require_auth(ctx, auth_token_); !st.ok()) return st;
    auto st = validate_module(req->module_id());
    if (!st.ok()) return st;

    spdlog::info("hal_service: RunSingleShot module_id={}", req->module_id());

    auto results = adapter_reg_->invoke(req->module_id(),
                                         hal::common::SINGLE_SHOT,
                                         module_config_bytes(req->module_config()));
    if (results.empty()) {
        resp->set_module_id(req->module_id());
        resp->set_mode(hal::common::SINGLE_SHOT);
        resp->set_status(hal::common::TEST_STATUS_SKIPPED);
        resp->set_error_message("adapter returned no results");
        return ::grpc::Status::OK;
    }

    // CLI-105: no longer silently discards results beyond the first — the
    // envelope carries the AGGREGATE verdict (worst status) and the first
    // detailed payload; multi-test results remain individually visible in the
    // batch/stream path.
    int worst = static_cast<int>(hal::common::TEST_STATUS_PASS);
    const hal::TestResult* worst_r = &results[0];
    std::string first_error;
    for (const auto& r : results) {
        const int s = static_cast<int>(hal_bridge::to_proto(r.output.status));
        if (s > worst) { worst = s; worst_r = &r; }
        if (!r.output.error_message.empty() && first_error.empty())
            first_error = r.output.error_message;
    }
    *resp = make_envelope(req->module_id(), hal::common::SINGLE_SHOT, *worst_r);
    resp->set_status(static_cast<hal::common::TestStatus>(worst));
    if (!first_error.empty())
        resp->set_error_message(first_error);
    return ::grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// StartLongTerm  (server-streaming)
// ---------------------------------------------------------------------------

::grpc::Status HalServiceImpl::StartLongTerm(
    ::grpc::ServerContext*                              ctx,
    const hal::LongTermRequest*                        req,
    ::grpc::ServerWriter<hal::common::ResultEnvelope>* writer)
{
    if (auto st = require_auth(ctx, auth_token_); !st.ok()) return st;
    auto st = validate_module(req->module_id());
    if (!st.ok()) return st;

    // CLI-106: enforce a floor — the HAL must never be hammered at 1 kHz.
    int interval_ms = req->common().interval_ms() > 0
                          ? req->common().interval_ms()
                          : kDefaultIntervalMs;
    if (interval_ms < 100) {
        interval_ms = 100;
        spdlog::warn("hal_service: interval_ms floored to 100 (was {})",
                     req->common().interval_ms());
    }
    const int64_t duration_ms  = req->common().duration_ms();
    const bool    run_forever  = req->common().run_forever() || duration_ms <= 0;

    // CLI-114/115: one shared streaming helper drives all three modes.
    return stream_loop(ctx, req->module_id(), req->module_config(),
                       hal::common::LONG_TERM, interval_ms, duration_ms, run_forever,
                       writer);
}

// ---------------------------------------------------------------------------
// StartMonitoring  (server-streaming)
// ---------------------------------------------------------------------------

::grpc::Status HalServiceImpl::StartMonitoring(
    ::grpc::ServerContext*                              ctx,
    const hal::MonitoringRequest*                      req,
    ::grpc::ServerWriter<hal::common::ResultEnvelope>* writer)
{
    if (auto st = require_auth(ctx, auth_token_); !st.ok()) return st;
    auto st = validate_module(req->module_id());
    if (!st.ok()) return st;

    int interval_ms = req->common().interval_ms() > 0
                          ? req->common().interval_ms()
                          : kDefaultIntervalMs;
    if (interval_ms < 100) {  // CLI-106
        interval_ms = 100;
        spdlog::warn("hal_service: interval_ms floored to 100 (was {})",
                     req->common().interval_ms());
    }
    const int64_t duration_ms  = req->common().duration_ms();
    const bool    run_forever  = req->common().run_forever() || duration_ms <= 0;

    // CLI-114/115: shared helper.
    return stream_loop(ctx, req->module_id(), req->module_config(),
                       hal::common::MONITORING, interval_ms, duration_ms, run_forever,
                       writer);
}

// ---------------------------------------------------------------------------
// StartEventBased  (server-streaming)
// ---------------------------------------------------------------------------

::grpc::Status HalServiceImpl::StartEventBased(
    ::grpc::ServerContext*                              ctx,
    const hal::EventBasedRequest*                      req,
    ::grpc::ServerWriter<hal::common::ResultEnvelope>* writer)
{
    if (auto st = require_auth(ctx, auth_token_); !st.ok()) return st;
    auto st = validate_module(req->module_id());
    if (!st.ok()) return st;

    int poll_ms = req->common().poll_interval_ms() > 0
                      ? req->common().poll_interval_ms()
                      : kDefaultPollMs;
    if (poll_ms < 100) {  // CLI-106: floor applies to the event poll too.
        poll_ms = 100;
        spdlog::warn("hal_service: poll_interval_ms floored to 100 (was {})",
                     req->common().poll_interval_ms());
    }
    const int64_t duration_ms  = req->common().duration_ms();
    const bool    run_forever  = req->common().run_forever() || duration_ms <= 0;

    // CLI-114/115: shared helper.
    return stream_loop(ctx, req->module_id(), req->module_config(),
                       hal::common::EVENT_BASED, poll_ms, duration_ms, run_forever,
                       writer);
}

// ---------------------------------------------------------------------------
// Shared streaming loop (CLI-114)
// ---------------------------------------------------------------------------

::grpc::Status HalServiceImpl::stream_loop(
    ::grpc::ServerContext*                     ctx,
    const std::string&                         module_id,
    const google::protobuf::Any&               module_config,
    hal::common::ExecutionMode                 mode,
    int                                        interval_ms,
    int64_t                                    duration_ms,
    bool                                       run_forever,
    ::grpc::ServerWriter<hal::common::ResultEnvelope>* writer)
{
    // CLI-113: serialize the config ONCE, not every tick.
    const std::vector<uint8_t> cfg_bytes = module_config_bytes(module_config);

    // CLI-111: bound concurrent streaming sessions.
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        if (sessions_.size() >= kMaxSessions) {
            return ::grpc::Status(
                ::grpc::StatusCode::RESOURCE_EXHAUSTED,
                "too many concurrent streaming sessions (limit " +
                    std::to_string(kMaxSessions) + ")");
        }
    }

    auto [session_id, session] = acquire_session(module_id);
    const auto t_start = std::chrono::steady_clock::now();

    // CLI-109: the session_id is announced in initial metadata so the caller
    // can address Stop/Pause/Resume at this specific session.
    ctx->AddInitialMetadata("x-acadia-session-id", session_id);
    spdlog::info("hal_service: stream module_id={} mode={} interval={}ms forever={} session={}",
                 module_id, static_cast<int>(mode), interval_ms, run_forever, session_id);

    // CLI-107: interval wait observes cancellation (condvar with timeout).
    std::mutex wake_mu;
    std::condition_variable wake_cv;

    while (!ctx->IsCancelled() && !session->stop_requested.load()) {
        if (!run_forever) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t_start).count();
            if (elapsed >= duration_ms) break;
        }
        if (session->paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPauseCheckMs));
            continue;
        }

        auto results = adapter_reg_->invoke(module_id, static_cast<int>(mode), cfg_bytes);
        for (const auto& r : results) {
            // CLI-108: a failed Write means the client is gone → CANCELLED.
            if (!writer->Write(make_envelope(module_id, mode, r))) {
                release_session(session_id);
                return ::grpc::Status{::grpc::StatusCode::CANCELLED, "client disconnected"};
            }
        }

        {
            std::unique_lock<std::mutex> wk(wake_mu);
            wake_cv.wait_for(wk, std::chrono::milliseconds(interval_ms),
                             [&] { return ctx->IsCancelled() || session->stop_requested.load(); });
        }
    }

    release_session(session_id);
    return ::grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// Stop / Pause / Resume
// ---------------------------------------------------------------------------

::grpc::Status HalServiceImpl::Stop(
    ::grpc::ServerContext* ctx,
    const hal::StopRequest*  req,
    hal::StopResponse*       resp)
{
    if (auto st = require_auth(ctx, auth_token_); !st.ok()) return st;
    spdlog::info("hal_service: Stop module_id={} session_id={}",
                 req->module_id(), req->session_id());
    auto targets = find_sessions(req->module_id(), req->session_id());
    if (!targets.empty()) {
        for (auto& s : targets) s->stop_requested.store(true);
        resp->set_accepted(true);
    } else {
        resp->set_accepted(false);
        resp->set_message("no active session for module_id=" + req->module_id());
    }
    return ::grpc::Status::OK;
}

::grpc::Status HalServiceImpl::Pause(
    ::grpc::ServerContext* ctx,
    const hal::PauseRequest*  req,
    hal::PauseResponse*       resp)
{
    if (auto st = require_auth(ctx, auth_token_); !st.ok()) return st;
    spdlog::info("hal_service: Pause module_id={}", req->module_id());
    auto targets = find_sessions(req->module_id(), req->session_id());
    if (!targets.empty()) {
        for (auto& s : targets) s->paused.store(true);
        resp->set_accepted(true);
    } else {
        resp->set_accepted(false);
        resp->set_message("no active session for module_id=" + req->module_id());
    }
    return ::grpc::Status::OK;
}

::grpc::Status HalServiceImpl::Resume(
    ::grpc::ServerContext* ctx,
    const hal::ResumeRequest*  req,
    hal::ResumeResponse*       resp)
{
    if (auto st = require_auth(ctx, auth_token_); !st.ok()) return st;
    spdlog::info("hal_service: Resume module_id={}", req->module_id());
    auto targets = find_sessions(req->module_id(), req->session_id());
    if (!targets.empty()) {
        for (auto& s : targets) s->paused.store(false);
        resp->set_accepted(true);
    } else {
        resp->set_accepted(false);
        resp->set_message("no active session for module_id=" + req->module_id());
    }
    return ::grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// ListModules
// ---------------------------------------------------------------------------

::grpc::Status HalServiceImpl::ListModules(
    ::grpc::ServerContext*          ctx,
    const hal::ListModulesRequest*  /*req*/,
    hal::ListModulesResponse*        resp)
{
    if (auto st = require_auth(ctx, auth_token_); !st.ok()) return st;
    spdlog::debug("hal_service: ListModules");
    for (const auto& m : module_reg_.all()) {
        auto* entry = resp->add_modules();
        entry->set_id(m.id);
        entry->set_proto_package(m.proto_package);
        entry->set_config_type(m.config_type_url);
        entry->set_result_type(m.result_type_url);
        // CLI-117: no exception-swallowing — validate characters, parse with
        // strtol, and surface non-numeric versions as 0 with a warning.
        {
            const std::string& v = m.version;
            bool numeric = !v.empty() && v.find_first_not_of("0123456789") == std::string::npos;
            if (numeric) {
                errno = 0;
                long parsed = std::strtol(v.c_str(), nullptr, 10);
                entry->set_version(errno == 0 ? static_cast<int32_t>(parsed) : 0);
            } else if (!v.empty()) {
                spdlog::warn("hal_service: module '{}' has non-numeric version '{}' — reporting 0", m.id, v);
                entry->set_version(0);
            }
        }
        // CLI-116: a stub-backed module is reported as unimplemented — not
        // active (it cannot execute real tests).
        const bool stub = adapter_reg_->has_adapter(m.id) &&
                          adapter_reg_->is_stub_adapter(m.id);
        entry->set_status(stub ? std::string("unimplemented")
                               : std::string(m.status.empty() ? "active" : m.status));
        entry->set_source_path(m.source_path);
    }
    return ::grpc::Status::OK;
}

} // namespace acadia::client::hal_service
