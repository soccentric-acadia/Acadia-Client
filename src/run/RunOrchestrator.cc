#include "run/RunOrchestrator.h"

#include "client_service.pb.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <stdexcept>

using namespace acadia::stream;

namespace acadia::client {

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------

RunOrchestrator::RunOrchestrator(Spool& spool,
                                   StreamClient& stream,
                                   HalInvokeFn hal_fn)
    : spool_(spool), stream_(stream), hal_fn_(std::move(hal_fn)) {}

RunOrchestrator::~RunOrchestrator() {
    stop_all();
}

// ---------------------------------------------------------------------------
// Server command handlers
// ---------------------------------------------------------------------------

void RunOrchestrator::on_configure(const ConfigureRun& msg) {
    const std::string& run_id = msg.run_id();
    spdlog::info("orchestrator: ConfigureRun run_id={} module={} mode={}",
                 run_id, msg.module_id(), static_cast<int>(msg.mode()));

    std::lock_guard<std::mutex> lk(mtx_);
    pending_configs_[run_id] = msg;

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
    const ConfigureRun& cfg = it->second;

    auto ctx = std::make_shared<RunContext>();
    ctx->run_id    = run_id;
    ctx->module_id = cfg.module_id();
    ctx->mode      = static_cast<int>(cfg.mode());
    ctx->type_url  = ""; // set per-module when known

    // Serialize module_config Any bytes for HAL invocation.
    cfg.module_config().SerializeToString(
        reinterpret_cast<std::string*>(nullptr)); // no-op placeholder
    ctx->config_blob.resize(cfg.module_config().ByteSizeLong());
    cfg.module_config().SerializeToArray(
        ctx->config_blob.data(),
        static_cast<int>(ctx->config_blob.size()));

    // Persist run state to spool.
    spool_.upsert_run_state(run_id, static_cast<int>(RunState::RUNNING), 0);

    RunEntry entry;
    entry.ctx    = ctx;
    entry.thread = std::thread([this, c = ctx]{ driver_thread(c); });
    runs_[run_id] = std::move(entry);
    pending_configs_.erase(it);

    spdlog::info("orchestrator: started run_id={}", run_id);
}

void RunOrchestrator::on_pause(const std::string& run_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (auto it = runs_.find(run_id); it != runs_.end())
        it->second.ctx->paused.store(true);
}

void RunOrchestrator::on_resume(const std::string& run_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (auto it = runs_.find(run_id); it != runs_.end())
        it->second.ctx->paused.store(false);
}

void RunOrchestrator::on_stop(const std::string& run_id) {
    std::unique_lock<std::mutex> lk(mtx_);
    if (auto it = runs_.find(run_id); it != runs_.end()) {
        it->second.ctx->stop.store(true);
        if (it->second.thread.joinable())
            it->second.thread.detach(); // Will finish naturally
        runs_.erase(it);
    }
    spool_.delete_run_state(run_id);
}

void RunOrchestrator::on_batch_ack(const std::string& run_id, int64_t batch_seq) {
    spool_.ack_batch(run_id, batch_seq);
}

void RunOrchestrator::stop_all() {
    std::unique_lock<std::mutex> lk(mtx_);
    for (auto& [id, entry] : runs_) {
        entry.ctx->stop.store(true);
        if (entry.thread.joinable()) entry.thread.join();
    }
    runs_.clear();
}

// ---------------------------------------------------------------------------
// Driver thread dispatch
// ---------------------------------------------------------------------------

void RunOrchestrator::driver_thread(std::shared_ptr<RunContext> ctx) {
    try {
        // acadia::stream::ExecutionMode values match common.proto
        switch (ctx->mode) {
            case 1: run_single_shot(*ctx); break;
            case 2: run_long_term(*ctx);   break;
            case 3: run_monitoring(*ctx);  break;
            case 4: run_event_based(*ctx); break;
            default:
                spdlog::error("orchestrator: unknown mode {} for run_id={}",
                              ctx->mode, ctx->run_id);
        }
    } catch (const std::exception& e) {
        spdlog::error("orchestrator: run_id={} driver exception: {}",
                      ctx->run_id, e.what());
    }
    spool_.upsert_run_state(ctx->run_id, static_cast<int>(RunState::COMPLETED),
                             ctx->batch_seq);
    spdlog::info("orchestrator: run_id={} driver finished", ctx->run_id);
}

// ---------------------------------------------------------------------------
// Flush one batch to spool + stream
// ---------------------------------------------------------------------------

void RunOrchestrator::flush_batch(RunContext& ctx,
                                   const std::vector<uint8_t>& sample_payload,
                                   bool terminal) {
    // Build TestResultBatch protobuf.
    TestResultBatch batch;
    batch.set_run_id(ctx.run_id);
    batch.set_batch_seq(++ctx.batch_seq);
    batch.set_module_id(ctx.module_id);
    batch.set_mode(static_cast<ExecutionMode>(ctx.mode));
    batch.set_state(terminal ? RunState::COMPLETED : RunState::RUNNING);
    batch.set_terminal(terminal);

    if (!sample_payload.empty()) {
        auto* s = batch.add_samples();
        auto* payload = s->mutable_payload();
        payload->set_type_url(ctx.type_url);
        payload->set_value(sample_payload.data(), sample_payload.size());
        s->set_status(hal::common::PASS);
    }

    // Serialize
    Bytes bytes(batch.ByteSizeLong());
    batch.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()));

    // Spool (durability) — always write even when backpressure blocks stream
    spool_.insert_batch(ctx.run_id, ctx.batch_seq, bytes);
    spool_.upsert_run_state(ctx.run_id, static_cast<int>(RunState::RUNNING),
                             ctx.batch_seq);

    // Send live on stream if connected
    ClientMessage msg;
    *msg.mutable_result_batch() = std::move(batch);
    stream_.send(std::move(msg));
}

// ---------------------------------------------------------------------------
// Single shot (T039)
// ---------------------------------------------------------------------------

void RunOrchestrator::run_single_shot(RunContext& ctx) {
    spdlog::info("orchestrator: single-shot run_id={} module={}",
                 ctx.run_id, ctx.module_id);

    auto result = hal_fn_(ctx.module_id, ctx.mode, ctx.config_blob);
    flush_batch(ctx, result, true);
}

// ---------------------------------------------------------------------------
// Long-term (T040) — 1 Hz loop
// ---------------------------------------------------------------------------

void RunOrchestrator::run_long_term(RunContext& ctx) {
    spdlog::info("orchestrator: long-term run_id={} module={}",
                 ctx.run_id, ctx.module_id);

    using clock = std::chrono::steady_clock;
    auto next_tick = clock::now() + std::chrono::seconds(1);

    while (!ctx.stop.load()) {
        // Backpressure: if spool is full, pause HAL
        while (spool_.result_spool_full() && !ctx.stop.load()) {
            spdlog::warn("orchestrator: spool full, pausing run_id={}", ctx.run_id);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        while (ctx.paused.load() && !ctx.stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (ctx.stop.load()) break;

        auto result = hal_fn_(ctx.module_id, ctx.mode, ctx.config_blob);
        flush_batch(ctx, result, false);

        // Sleep until next 1 Hz tick
        std::this_thread::sleep_until(next_tick);
        next_tick += std::chrono::seconds(1);
    }

    flush_batch(ctx, {}, true);
}

// ---------------------------------------------------------------------------
// Monitoring (T041) — same as long-term but mode=MONITORING
// ---------------------------------------------------------------------------

void RunOrchestrator::run_monitoring(RunContext& ctx) {
    spdlog::info("orchestrator: monitoring run_id={} module={}",
                 ctx.run_id, ctx.module_id);
    // Monitoring mode: identical loop to long-term; the HAL itself
    // enriches the result with monitoring params.
    run_long_term(ctx);
}

// ---------------------------------------------------------------------------
// Event-based (T042)
// ---------------------------------------------------------------------------

void RunOrchestrator::run_event_based(RunContext& ctx) {
    spdlog::info("orchestrator: event-based run_id={} module={}",
                 ctx.run_id, ctx.module_id);

    using clock = std::chrono::steady_clock;
    auto next_keepalive = clock::now() + std::chrono::seconds(1);

    while (!ctx.stop.load()) {
        while (ctx.paused.load() && !ctx.stop.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (ctx.stop.load()) break;

        // HAL returns non-empty bytes only when the condition fires.
        auto result = hal_fn_(ctx.module_id, ctx.mode, ctx.config_blob);

        auto now = clock::now();
        if (!result.empty()) {
            // Condition fired — send result immediately
            flush_batch(ctx, result, false);
            next_keepalive = now + std::chrono::seconds(1);
        } else if (now >= next_keepalive) {
            // Keepalive — empty batch to signal we're alive
            flush_batch(ctx, {}, false);
            next_keepalive = now + std::chrono::seconds(1);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    flush_batch(ctx, {}, true);
}

} // namespace acadia::client
