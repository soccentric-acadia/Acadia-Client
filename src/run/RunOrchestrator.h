#pragma once
// run/RunOrchestrator.h — manages per-run driver threads.
//
// When the Server pushes a ConfigureRun, the orchestrator validates it,
// creates a RunDriver for the appropriate mode, and starts it on a new
// thread.  All runs multiplex through the StreamClient.
//
// Thread safety: all public methods are thread-safe.

#include "spool/Spool.h"
#include "stream/StreamClient.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace acadia::stream {
    class ConfigureRun;
    class TestResultBatch;
}

namespace acadia::client {

// Opaque HAL invocation callback — in production, wired to the HAL library.
// Signature: (module_id, mode, config_bytes) → result_bytes.
// Returning empty bytes signals HAL error.
using HalInvokeFn = std::function<
    std::vector<uint8_t>(const std::string& module_id,
                          int mode,
                          const std::vector<uint8_t>& config_bytes)>;

struct RunContext {
    std::string              run_id;
    std::string              module_id;
    int                      mode        = 0;  // acadia::stream::ExecutionMode
    std::vector<uint8_t>     config_blob;      // serialised Any
    std::atomic<bool>        paused{false};
    std::atomic<bool>        stop{false};
    int64_t                  batch_seq   = 0;
    std::string              type_url;         // result type URL
};

class RunOrchestrator {
public:
    RunOrchestrator(Spool& spool, StreamClient& stream, HalInvokeFn hal_fn);
    ~RunOrchestrator();

    RunOrchestrator(const RunOrchestrator&)            = delete;
    RunOrchestrator& operator=(const RunOrchestrator&) = delete;

    // Handle incoming server commands (called from stream thread).
    void on_configure(const acadia::stream::ConfigureRun& msg);
    void on_start(const std::string& run_id);
    void on_pause(const std::string& run_id);
    void on_resume(const std::string& run_id);
    void on_stop(const std::string& run_id);

    // Handle ACK from server.
    void on_batch_ack(const std::string& run_id, int64_t batch_seq);

    // Stop all active runs (shutdown).
    void stop_all();

private:
    struct RunEntry {
        std::shared_ptr<RunContext> ctx;
        std::thread                 thread;
    };

    void driver_thread(std::shared_ptr<RunContext> ctx);
    void run_single_shot(RunContext& ctx);
    void run_long_term(RunContext& ctx);
    void run_monitoring(RunContext& ctx);
    void run_event_based(RunContext& ctx);

    void flush_batch(RunContext& ctx,
                     const std::vector<uint8_t>& sample_payload,
                     bool terminal);

    Spool&          spool_;
    StreamClient&   stream_;
    HalInvokeFn     hal_fn_;

    mutable std::mutex mtx_;
    std::unordered_map<std::string, RunEntry> runs_;
    std::unordered_map<std::string, acadia::stream::ConfigureRun> pending_configs_;
};

} // namespace acadia::client
