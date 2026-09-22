#include "run/RunOrchestrator.h"

#include "hal_bridge/ResultSerializer.h"
#include "client_service.pb.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <future>
#include <stdexcept>

using namespace acadia::stream;

namespace acadia::client {

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------

RunOrchestrator::RunOrchestrator(Spool& spool,
                                   StreamClient& stream,
                                   HalInvokeFn hal_fn,
                                   std::function<bool(const std::string&)> has_module)
    : spool_(spool), stream_(stream), hal_fn_(std::move(hal_fn)),
      has_module_(std::move(has_module)) {
    // CLI-097: reconcile RUNNING runs from a previous process — they were not
    // resumed (the HAL sessions are gone), so they are failed locally with a
    // clear reason rather than advertised as active in Hello forever.
    for (const auto& rs : spool_.all_run_states()) {
        if (rs.state == static_cast<int>(RunState::RUNNING)) {
            spool_.upsert_run_state(rs.run_id, static_cast<int>(RunState::FAILED), rs.last_seq);
            spdlog::warn("orchestrator: reconciled RUNNING run_id={} to FAILED on boot",
                         rs.run_id);
        }
    }
}

RunOrchestrator::~RunOrchestrator() {
    stop_all();
}

// ---------------------------------------------------------------------------
// Server command handlers
// ---------------------------------------------------------------------------

void RunOrchestrator::on_configure(const ConfigureRun& msg) {
    const std::string& run_id = msg.run_id();
    const int mode = static_cast<int>(msg.mode());
    spdlog::info("orchestrator: ConfigureRun run_id={} module={} mode={}",
                 run_id, msg.module_id(), mode);

    // Task 72: validate the execution mode BEFORE starting the run. An unknown
    // or future enum value must produce an explicit protocol error and a failed
    // run state — never be persisted as a completed run without an error result.
    const bool mode_valid = (mode == 1 || mode == 2 || mode == 3 || mode == 4);
    if (!mode_valid) {
        spdlog::error("orchestrator: ConfigureRun rejected — unknown execution mode {} for run_id={}",
                      mode, run_id);
        spool_.upsert_run_state(run_id, static_cast<int>(RunState::FAILED), 0);
        ClientMessage err_msg;
        auto* ack = err_msg.mutable_configure_ack();
        ack->set_run_id(run_id);
        ack->set_ok(false);
        ack->set_error("unknown execution mode: " + std::to_string(mode));
        stream_.send(std::move(err_msg));
        return;
    }

    std::lock_guard<std::mutex> lk(mtx_);

    // CLI-088: reject at configure time when no adapter exists for the module
    // (previously ACKed ok=true and later produced a StubAdapter SKIPPED).
    if (has_module_ && !has_module_(msg.module_id())) {
        spdlog::error("orchestrator: ConfigureRun rejected — no adapter for module_id={}",
                      msg.module_id());
        spool_.upsert_run_state(run_id, static_cast<int>(RunState::FAILED), 0);
        ClientMessage err;
        auto* ack = err.mutable_configure_ack();
        ack->set_run_id(run_id);
        ack->set_ok(false);
        ack->set_error("unknown module_id: " + msg.module_id());
        stream_.send(std::move(err));
        return;
    }

    // CLI-087: expire pending configs whose StartRun never arrived.
    const auto now = std::chrono::steady_clock::now();
    for (auto it = pending_configs_.begin(); it != pending_configs_.end();) {
        if (now - it->second.second > kPendingTtl) {
            spdlog::warn("orchestrator: pending Configure run_id={} expired (TTL {}s)",
                         it->first, kPendingTtl.count());
            it = pending_configs_.erase(it);
        } else {
            ++it;
        }
    }

    // CLI-086: cap concurrent runs.
    if (runs_.size() >= kMaxConcurrentRuns) {
        spdlog::error("orchestrator: run cap ({}) reached — rejecting run_id={}",
                      kMaxConcurrentRuns, run_id);
        spool_.upsert_run_state(run_id, static_cast<int>(RunState::FAILED), 0);
        ClientMessage err;
        auto* ack = err.mutable_configure_ack();
        ack->set_run_id(run_id);
        ack->set_ok(false);
        ack->set_error("too many concurrent runs");
        stream_.send(std::move(err));
        return;
    }

    pending_configs_[run_id] = {msg, now};

    // Send ACK back to server.
    ClientMessage ack_msg;
    auto* ack = ack_msg.mutable_configure_ack();
    ack->set_run_id(run_id);
    ack->set_ok(true);
    stream_.send(std::move(ack_msg));
}

void RunOrchestrator::on_start(const std::string& run_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = pending_configs_.find(run_id);
    if (it == pending_configs_.end()) {
        spdlog::warn("orchestrator: StartRun for unknown run_id={}", run_id);
        return;
    }
    const ConfigureRun& cfg = it->second.first;

    // CLI-085: reap finished workers before deciding duplication (a finished
    // run's entry is joined and erased, freeing the run_id).
    reap_finished_locked();

    // CLI-082: reject a duplicate StartRun for a run_id that is still live.
    if (runs_.find(run_id) != runs_.end()) {
        spdlog::error("orchestrator: duplicate StartRun rejected for live run_id={}",
                      run_id);
        spool_.upsert_run_state(run_id, static_cast<int>(RunState::FAILED), 0);
        ClientMessage err_msg;
        auto* ack = err_msg.mutable_configure_ack();
        ack->set_run_id(run_id);
        ack->set_ok(false);
        ack->set_error("duplicate run_id already running");
        stream_.send(std::move(err_msg));
        return;
    }

    auto ctx = std::make_shared<RunContext>();
    ctx->run_id    = run_id;
    ctx->module_id = cfg.module_id();
    ctx->mode      = static_cast<int>(cfg.mode());

    // CLI-092/093: timing from ConfigureRun (fallback 1 s / 200 ms).
    ctx->interval_ms = cfg.common().interval_ms() > 0 ? cfg.common().interval_ms() : 1000;
    ctx->poll_ms     = cfg.common().poll_interval_ms() > 0 ? cfg.common().poll_interval_ms() : 200;

    // Serialize module_config Any bytes for HAL invocation.
    ctx->config_blob.resize(cfg.module_config().ByteSizeLong());
    cfg.module_config().SerializeToArray(
        ctx->config_blob.data(),
        static_cast<int>(ctx->config_blob.size()));

    // CLI-098: resume the batch sequence from the spool's persisted last_seq —
    // a restarted process must not restart at 1 and collide with spooled rows.
    int64_t last_seq = 0;
    for (const auto& rs : spool_.all_run_states()) {
        if (rs.run_id == run_id) { last_seq = rs.last_seq; break; }
    }
    ctx->batch_seq = last_seq;

    // Persist run state to spool (CLI-096: PAUSED start persisted too).
    spool_.upsert_run_state(run_id, static_cast<int>(RunState::RUNNING), last_seq);

    RunEntry entry;
    entry.ctx    = ctx;
    entry.thread = std::thread([this, c = ctx]{ driver_thread(c); });
    runs_[run_id] = std::move(entry);
    pending_configs_.erase(it);

    spdlog::info("orchestrator: started run_id={}", run_id);
}

void RunOrchestrator::on_pause(const std::string& run_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    // CLI-096: persist the PAUSED state so Hello reports it correctly.
    if (auto it = runs_.find(run_id); it != runs_.end()) {
        it->second.ctx->paused.store(true);
        spool_.upsert_run_state(run_id, static_cast<int>(RunState::PAUSED),
                                it->second.ctx->batch_seq);
    }
}

void RunOrchestrator::on_resume(const std::string& run_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (auto it = runs_.find(run_id); it != runs_.end()) {
        it->second.ctx->paused.store(false);
        spool_.upsert_run_state(run_id, static_cast<int>(RunState::RUNNING),
                                it->second.ctx->batch_seq);
    }
}

void RunOrchestrator::on_stop(const std::string& run_id) {
    // Task 69: join the worker before erasing so it never accesses owner state
    // (spool_/sender_) after destruction. Previously the thread was detached
    // and the run erased immediately, leaving a use-after-free window.
    std::thread worker;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (auto it = runs_.find(run_id); it != runs_.end()) {
            it->second.ctx->stop.store(true);
            if (it->second.thread.joinable()) {
                worker = std::move(it->second.thread);
                have   = true;
            }
            runs_.erase(it);
        }
    }
    // Join OUTSIDE the lock: the worker may need the lock or spool during its
    // final cleanup; holding mtx_ here would deadlock.
    if (have && worker.joinable()) worker.join();
    spool_.delete_run_state(run_id);
}

void RunOrchestrator::on_batch_ack(const std::string& run_id, int64_t batch_seq) {
    spool_.ack_batch(run_id, batch_seq);
    // CLI-085: a finished worker is reaped at the next opportunity (batch acks
    // arrive for every batch, so finished single-shot runs never linger).
    std::lock_guard<std::mutex> lk(mtx_);
    reap_finished_locked();
}

void RunOrchestrator::stop_all() {
    std::vector<std::thread> workers;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        for (auto& [id, entry] : runs_) {
            entry.ctx->stop.store(true);
            if (entry.thread.joinable()) workers.push_back(std::move(entry.thread));
        }
        runs_.clear();
    }
    // Task 69: join outside the lock so workers' final cleanup can proceed.
    for (auto& w : workers) if (w.joinable()) w.join();
}

// CLI-085: join and erase drivers whose thread has finished.
void RunOrchestrator::reap_finished_locked() {
    for (auto it = runs_.begin(); it != runs_.end();) {
        auto& entry = it->second;
        // A worker that already wrote its terminal spool state marks itself
        // done by setting failed/aborted/stop + a COMPLETED upsert; the
        // thread is joinable until joined — join without blocking by probing
        // a finished flag.
        if (entry.ctx->stop.load() && entry.thread.joinable()) {
            // stop is only set on cancel; normal completion is detected via
            // the driver_finished flag below.
        }
        if (entry.ctx->finished.load() && entry.thread.joinable()) {
            entry.thread.join();
            it = runs_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Driver thread dispatch
// ---------------------------------------------------------------------------

void RunOrchestrator::driver_thread(std::shared_ptr<RunContext> ctx) {
    try {
        // CLI-100: validate with the generated enum (no magic numbers).
        if (!hal::common::ExecutionMode_IsValid(ctx->mode)) {
            spdlog::error("orchestrator: unknown mode {} for run_id={}",
                          ctx->mode, ctx->run_id);
            ctx->failed.store(true);
        } else {
            switch (ctx->mode) {
                case 1: run_single_shot(*ctx); break;
                case 2:   run_long_term(*ctx);   break;
                case 3:  run_monitoring(*ctx);  break;
                case 4: run_event_based(*ctx); break;
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("orchestrator: run_id={} driver exception: {}",
                      ctx->run_id, e.what());
        ctx->failed.store(true);
    }
    // CLI-083: preserve an already-terminal state — flush_synthetic (FAILED on
    // spool-insert failure) or an explicit ABORTED must not be overwritten
    // with COMPLETED.
    if (ctx->failed.load()) {
        spool_.upsert_run_state(ctx->run_id, static_cast<int>(RunState::FAILED),
                                ctx->batch_seq);
    } else if (ctx->aborted.load()) {
        spool_.upsert_run_state(ctx->run_id, static_cast<int>(RunState::CANCELLED),
                                ctx->batch_seq);
    } else {
        spool_.upsert_run_state(ctx->run_id, static_cast<int>(RunState::COMPLETED),
                                ctx->batch_seq);
    }
    ctx->finished.store(true);
    spdlog::info("orchestrator: run_id={} driver finished", ctx->run_id);
}

// ---------------------------------------------------------------------------
// Flush results — stamp sequence + timestamp, serialize, spool, stream
// ---------------------------------------------------------------------------

void RunOrchestrator::flush_results(RunContext& ctx,
                                     std::vector<hal::TestResult> results,
                                     bool terminal) {
    using clock = std::chrono::system_clock;

    // Stamp sequence and timestamp on every result (spec: runner is the only
    // place that sets these). CLI-090: a HAL result carries its own sample
    // timestamp — use it when present, falling back to now().
    for (auto& r : results) {
        r.sequence  = ctx.next_sequence++;
        if (r.timestamp.time_since_epoch().count() == 0) {
            r.timestamp = clock::now();
        }
    }

    // Build TestResultBatch protobuf.
    TestResultBatch batch;
    batch.set_run_id(ctx.run_id);
    batch.set_batch_seq(++ctx.batch_seq);
    batch.set_module_id(ctx.module_id);
    batch.set_mode(hal_bridge::execution_mode_from_int(ctx.mode));
    // CLI-084: report the ACTUAL terminal state (FAILED on error, CANCELLED
    // when aborted) instead of always COMPLETED.
    RunState batch_state;
    if (terminal && ctx.failed.load()) batch_state = RunState::FAILED;
    else if (terminal && ctx.aborted.load()) batch_state = RunState::CANCELLED;
    else batch_state = terminal ? RunState::COMPLETED : RunState::RUNNING;
    batch.set_state(batch_state);
    batch.set_terminal(terminal);

    for (const auto& r : results) {
        auto* sample = batch.add_samples();
        *sample->mutable_timestamp() = hal_bridge::to_proto(r.timestamp);
        sample->set_status(hal_bridge::to_proto(r.output.status));
        if (!r.output.error_message.empty())
            sample->set_error(r.output.error_message);
        // Pack the full TestResult into the Any payload.
        hal::common::TestResult proto_result = hal_bridge::to_proto(r);
        sample->mutable_payload()->PackFrom(proto_result);
        sample->set_type_url("type.googleapis.com/hal.common.TestResult");
    }

    // Serialize.
    Bytes bytes(batch.ByteSizeLong());
    batch.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()));

    // Backpressure: block if spool is full (never silently drop).
    // CLI-095: log at most once per 5 s while blocked.
    auto last_bp_log = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    while (spool_.result_spool_full() && !ctx.stop.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_bp_log > std::chrono::seconds(5)) {
            spdlog::warn("orchestrator: spool full — applying backpressure for run_id={}",
                         ctx.run_id);
            last_bp_log = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Task 73: propagate insert_batch failure — a full or oversized spool must
    // surface as a FAILED run with operator-visible diagnostics, never a silent
    // drop that leaves the batch un-acked.
    if (!spool_.insert_batch(ctx.run_id, ctx.batch_seq, bytes)) {
        spdlog::error("orchestrator: spool insert_batch FAILED for run_id={} seq={} "
                      "(full or oversized); marking run failed",
                      ctx.run_id, ctx.batch_seq);
        spool_.upsert_run_state(ctx.run_id, static_cast<int>(RunState::FAILED),
                                ctx.batch_seq);
        ctx.failed.store(true);  // CLI-083: FAILED is terminal — preserved at exit
        ClientMessage err_msg;
        auto* batch = err_msg.mutable_result_batch();
        batch->set_run_id(ctx.run_id);
        batch->set_batch_seq(ctx.batch_seq);
        batch->set_module_id(ctx.module_id);
        batch->set_mode(hal_bridge::execution_mode_from_int(ctx.mode));
        // CLI-084: this synthetic batch carries the FAILED state itself.
        batch->set_state(RunState::FAILED);
        batch->set_terminal(true);
        stream_.send(std::move(err_msg));
        return;
    }
    spool_.upsert_run_state(ctx.run_id, static_cast<int>(RunState::RUNNING),
                             ctx.batch_seq);

    ClientMessage msg;
    *msg.mutable_result_batch() = std::move(batch);
    stream_.send(std::move(msg));
}

void RunOrchestrator::flush_synthetic(RunContext& ctx,
                                       hal::Status status,
                                       const std::string& error_message,
                                       bool terminal) {
    hal::TestResult r;
    r.module_id           = ctx.module_id;
    r.test_id             = ctx.module_id;
    r.name                = ctx.module_id;
    r.output.status       = status;
    r.output.error_message = error_message;
    using sev = hal::Severity;
    switch (status) {
        case hal::Status::TIMEOUT: r.output.severity = sev::ERROR;    break;
        case hal::Status::ABORTED: r.output.severity = sev::WARNING;  break;
        case hal::Status::ERROR:   r.output.severity = sev::ERROR;    break;
        default:                   r.output.severity = sev::INFO;     break;
    }
    flush_results(ctx, {std::move(r)}, terminal);
}

// ---------------------------------------------------------------------------
// Single shot (T039)
// ---------------------------------------------------------------------------

void RunOrchestrator::run_single_shot(RunContext& ctx) {
    spdlog::info("orchestrator: single-shot run_id={} module={}",
                 ctx.run_id, ctx.module_id);

    // SINGLE_SHOT: sequence = 0, timestamp = now() after HAL returns.
    // CLI-094: bounded by a watchdog so a hung HAL call cannot hang the run.
    try {
        auto results = hal_call_watchdog(ctx, std::chrono::milliseconds(30000));
        if (results.empty()) {
            flush_synthetic(ctx, hal::Status::ERROR,
                            "HAL returned empty results for single-shot", true);
        } else {
            flush_results(ctx, std::move(results), true);
        }
    } catch (const std::exception& e) {
        const bool timed_out = std::string(e.what()).find("timed out") != std::string::npos;
        spdlog::error("orchestrator: single-shot HAL exception run_id={}: {}",
                      ctx.run_id, e.what());
        flush_synthetic(ctx, timed_out ? hal::Status::TIMEOUT : hal::Status::ERROR,
                        timed_out ? "HAL call timed out" :
                                    std::string("HAL exception: ") + e.what(), true);
    }
}

// ---------------------------------------------------------------------------
// Long-term (T040) — 1 Hz loop
// ---------------------------------------------------------------------------

void RunOrchestrator::run_long_term(RunContext& ctx) {
    spdlog::info("orchestrator: long-term run_id={} module={}",
                 ctx.run_id, ctx.module_id);

    using clock = std::chrono::steady_clock;
    // CLI-092: cycle interval from ConfigureRun.
    const auto interval = std::chrono::milliseconds(ctx.interval_ms);
    auto next_tick = clock::now() + interval;

    while (!ctx.stop.load()) {
        // Backpressure: block if spool is full (CLI-095: rate-limited log).
        auto last_bp_log = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        while (spool_.result_spool_full() && !ctx.stop.load()) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_bp_log > std::chrono::seconds(5)) {
                spdlog::warn("orchestrator: spool full, pausing run_id={}", ctx.run_id);
                last_bp_log = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        while (ctx.paused.load() && !ctx.stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (ctx.stop.load()) break;

        try {
            // CLI-094: watchdog around the per-tick HAL call.
            auto results = hal_call_watchdog(ctx, std::chrono::milliseconds(30000));
            flush_results(ctx, std::move(results), false);
        } catch (const std::exception& e) {
            const bool timed_out = std::string(e.what()).find("timed out") != std::string::npos;
            spdlog::error("orchestrator: long-term HAL exception run_id={}: {}",
                          ctx.run_id, e.what());
            flush_synthetic(ctx, timed_out ? hal::Status::TIMEOUT : hal::Status::ERROR,
                            timed_out ? "HAL call timed out" :
                                        std::string("HAL exception: ") + e.what(), false);
        }

        // Sleep until the next tick; CLI-091: a slow HAL call must not fire a
        // burst of catch-up iterations — skip the missed ticks by resyncing.
        std::this_thread::sleep_until(next_tick);
        auto now = clock::now();
        if (now >= next_tick + interval) {
            // Missed more than one tick: resync instead of catch-up-bursting.
            next_tick = now + interval;
        } else {
            next_tick += interval;
        }
    }

    // Terminal batch: CLI-084 — CANCELLED (ABORTED) if stop was requested
    // mid-run; the batch state now reflects that.
    ctx.aborted.store(true);
    flush_synthetic(ctx, hal::Status::ABORTED, "cancelled by server", true);
}

// ---------------------------------------------------------------------------
// Monitoring (T041) — same loop as long-term; HAL enriches monitoring map
// ---------------------------------------------------------------------------

void RunOrchestrator::run_monitoring(RunContext& ctx) {
    spdlog::info("orchestrator: monitoring run_id={} module={}",
                 ctx.run_id, ctx.module_id);
    // Monitoring mode is identical to long-term from the runner's perspective;
    // the HAL adapter populates output.monitoring for MONITORING mode results.
    run_long_term(ctx);
}

// ---------------------------------------------------------------------------
// Event-based (T042)
// ---------------------------------------------------------------------------

// CLI-094: invoke the HAL with a watchdog — a hung HAL call cannot hang the
// run (and the StopRun join) forever; it is converted to a TIMEOUT result.
// The watchdog thread owns COPIES of the invocation inputs so it can outlive
// the run context safely when detached after a timeout.
std::vector<hal::TestResult> RunOrchestrator::hal_call_watchdog(
    RunContext& ctx, std::chrono::milliseconds timeout_ms) {
    const std::string module_id = ctx.module_id;
    const int mode = ctx.mode;
    const std::vector<uint8_t> config_blob = ctx.config_blob;
    HalInvokeFn fn = hal_fn_;

    std::packaged_task<std::vector<hal::TestResult>()> task(
        [fn, module_id, mode, cfg = config_blob] {
            return fn(module_id, mode, cfg);
        });
    auto fut = task.get_future();
    std::thread worker(std::move(task));
    if (fut.wait_for(timeout_ms) == std::future_status::ready) {
        auto result = fut.get();
        return result;
    }
    // Timeout: detach the worker (the HAL call may still be blocked inside a
    // driver; it will finish or fail on its own) and report TIMEOUT so the
    // run surfaces the hang instead of stalling forever.
    spdlog::error("orchestrator: HAL call timed out run_id={} (watchdog detached the call)",
                  ctx.run_id);
    worker.detach();
    throw std::runtime_error("hal call timed out");
}

void RunOrchestrator::run_event_based(RunContext& ctx) {
    spdlog::info("orchestrator: event-based run_id={} module={}",
                 ctx.run_id, ctx.module_id);

    using clock = std::chrono::steady_clock;
    auto next_keepalive = clock::now() + std::chrono::seconds(1);
    // CLI-093: poll period from ConfigureRun (previously 10 ms = 100 Hz busy
    // poll burning a core and hammering the HAL).
    const auto poll = std::chrono::milliseconds(ctx.poll_ms);

    while (!ctx.stop.load()) {
        while (ctx.paused.load() && !ctx.stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (ctx.stop.load()) break;

        try {
            // HAL returns non-empty results only when the condition fires.
            // CLI-094: watchdog-bounded like the other modes.
            auto results = hal_call_watchdog(ctx, std::chrono::milliseconds(30000));

            auto now = clock::now();
            if (!results.empty()) {
                flush_results(ctx, std::move(results), false);
                next_keepalive = now + std::chrono::seconds(1);
            } else if (now >= next_keepalive) {
                // Keepalive: empty terminal=false batch to signal liveness.
                flush_results(ctx, {}, false);
                next_keepalive = now + std::chrono::seconds(1);
            }
        } catch (const std::exception& e) {
            spdlog::error("orchestrator: event-based HAL exception run_id={}: {}",
                          ctx.run_id, e.what());
            flush_synthetic(ctx, hal::Status::ERROR,
                            std::string("HAL exception: ") + e.what(), false);
        }

        std::this_thread::sleep_for(poll);
    }

    ctx.aborted.store(true);
    flush_synthetic(ctx, hal::Status::ABORTED, "cancelled by server", true);
}

} // namespace acadia::client
