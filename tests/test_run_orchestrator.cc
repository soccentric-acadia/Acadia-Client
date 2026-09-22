// tests/test_run_orchestrator.cc — Integration tests for RunOrchestrator.
//
// Per Section 5.3 of req.md:
//  - SINGLE_SHOT: sequence == 0, timestamp set, exactly one result batch
//  - LONG_TERM: monotonic sequences, timestamps monotonic-ish
//  - TIMEOUT synthesis: HAL throws → status == TIMEOUT
//  - ABORTED synthesis: stop requested → final result status == ABORTED
//
// Uses an in-memory Spool (:memory:) and a no-op StreamClient stub.

#include <gtest/gtest.h>

#include "run/RunOrchestrator.h"
#include "hal_bridge/ResultSerializer.h"
#include "client_service.pb.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>
#include <atomic>

namespace {

using namespace acadia::client;
using namespace acadia::stream;

// ─── Minimal Spool (in-memory) ────────────────────────────────────────────────

std::unique_ptr<Spool> make_mem_spool() {
    static std::atomic<int> counter{0};
    // Unique per process+counter so stale DBs from earlier runs can never
    // collide (the tests assume a fresh spool).
    auto path = std::filesystem::temp_directory_path() /
                ("test_spool_" + std::to_string(::getpid()) + "_" +
                 std::to_string(counter++) + ".db");
    std::filesystem::remove(path);
    std::filesystem::remove(path.generic_string().append("-wal"));
    std::filesystem::remove(path.generic_string().append("-shm"));
    return std::make_unique<Spool>(path.string(), 64 * 1024 * 1024, 16 * 1024 * 1024);
}

// ─── No-op StreamClient stub ─────────────────────────────────────────────────
// We use the real StreamClient but don't connect; send() returns false.
// Batches are still spooled so we can read them back.

StreamClient make_stub_stream() {
    StreamClientConfig cfg;
    cfg.server_address = "127.0.0.1";
    cfg.server_port    = 19999;  // Nothing listening here
    return StreamClient{cfg, [](const ServerMessage&) {}};
}

// ─── HAL stubs ────────────────────────────────────────────────────────────────

// Returns a single PASS result per call.
static std::vector<hal::TestResult> hal_stub_pass(
    const std::string& module_id, int /*mode*/, const std::vector<uint8_t>&)
{
    hal::TestResult r;
    r.module_id     = module_id;
    r.test_id       = "stub_test";
    r.name          = "Stub Test";
    r.output.status = hal::Status::PASS;
    r.output.severity = hal::Severity::INFO;
    r.output.metrics["value"] = int64_t{1};
    return {r};
}

// Throws to simulate a HAL exception / timeout.
static std::vector<hal::TestResult> hal_stub_throw(
    const std::string&, int, const std::vector<uint8_t>&)
{
    throw std::runtime_error("simulated HAL timeout");
}

// Returns empty (EVENT_BASED: no event fired).
// (hal_stub_empty removed — unused; EVENT_BASED empty results come from the
// adapter when the condition is not met.)

// ─── Helpers ─────────────────────────────────────────────────────────────────

ConfigureRun make_configure(const std::string& run_id,
                             const std::string& module_id = "cpu",
                             int mode = 1 /* SINGLE_SHOT */) {
    ConfigureRun cfg;
    cfg.set_run_id(run_id);
    cfg.set_module_id(module_id);
    cfg.set_mode(static_cast<hal::common::ExecutionMode>(mode));
    return cfg;
}

// ─── SINGLE_SHOT tests ────────────────────────────────────────────────────────

TEST(RunOrchestrator, SingleShotSequenceIsZero) {
    auto spool = make_mem_spool();
    auto stream = make_stub_stream();
    RunOrchestrator orch(*spool, stream, hal_stub_pass);

    orch.on_configure(make_configure("run1", "cpu", 1));
    orch.on_start("run1");
    // Give the driver thread time to complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    orch.stop_all();

    auto batches = spool->fetch_unacked(10);
    ASSERT_FALSE(batches.empty());

    // Deserialize first batch and verify sequence == 0.
    TestResultBatch batch;
    ASSERT_TRUE(batch.ParseFromArray(batches[0].payload.data(),
                                     static_cast<int>(batches[0].payload.size())));
    ASSERT_GE(batch.samples_size(), 1);
    // Unpack the TestResult from the Any payload.
    hal::common::TestResult proto_result;
    ASSERT_TRUE(batch.samples(0).payload().UnpackTo(&proto_result));
    EXPECT_EQ(proto_result.sequence(), 0u);
}

TEST(RunOrchestrator, SingleShotTimestampSet) {
    auto spool = make_mem_spool();
    auto stream = make_stub_stream();
    RunOrchestrator orch(*spool, stream, hal_stub_pass);

    auto before = std::chrono::system_clock::now();
    orch.on_configure(make_configure("run2", "cpu", 1));
    orch.on_start("run2");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    orch.stop_all();
    auto after = std::chrono::system_clock::now();

    auto batches = spool->fetch_unacked(10);
    ASSERT_FALSE(batches.empty());

    TestResultBatch batch;
    ASSERT_TRUE(batch.ParseFromArray(batches[0].payload.data(),
                                     static_cast<int>(batches[0].payload.size())));
    ASSERT_GE(batch.samples_size(), 1);

    hal::common::TestResult proto_result;
    ASSERT_TRUE(batch.samples(0).payload().UnpackTo(&proto_result));

    auto ts = acadia::client::hal_bridge::from_proto(proto_result.timestamp());
    EXPECT_GE(ts, before);
    EXPECT_LE(ts, after);
}

TEST(RunOrchestrator, SingleShotTerminalBatch) {
    auto spool = make_mem_spool();
    auto stream = make_stub_stream();
    RunOrchestrator orch(*spool, stream, hal_stub_pass);

    orch.on_configure(make_configure("run3", "cpu", 1));
    orch.on_start("run3");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    orch.stop_all();

    auto batches = spool->fetch_unacked(10);
    ASSERT_FALSE(batches.empty());
    TestResultBatch batch;
    ASSERT_TRUE(batch.ParseFromArray(batches[0].payload.data(),
                                     static_cast<int>(batches[0].payload.size())));
    EXPECT_TRUE(batch.terminal());
}

// ─── HAL exception → ERROR synthesis ─────────────────────────────────────────

TEST(RunOrchestrator, HalExceptionProducesErrorResult) {
    auto spool = make_mem_spool();
    auto stream = make_stub_stream();
    RunOrchestrator orch(*spool, stream, hal_stub_throw);

    orch.on_configure(make_configure("run_err", "cpu", 1));
    orch.on_start("run_err");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    orch.stop_all();

    auto batches = spool->fetch_unacked(10);
    ASSERT_FALSE(batches.empty());

    TestResultBatch batch;
    ASSERT_TRUE(batch.ParseFromArray(batches[0].payload.data(),
                                     static_cast<int>(batches[0].payload.size())));
    ASSERT_GE(batch.samples_size(), 1);
    EXPECT_EQ(batch.samples(0).status(), hal::common::TEST_STATUS_ERROR);
}

// ─── Monotonic sequences for LONG_TERM ────────────────────────────────────────

TEST(RunOrchestrator, LongTermMonotonicSequence) {
    auto spool = make_mem_spool();
    auto stream = make_stub_stream();

    // Use a counter to verify monotonicity.
    std::vector<uint64_t> sequences;
    std::mutex seq_mtx;

    auto hal_counting = [&](const std::string& module_id, int mode,
                             const std::vector<uint8_t>& cfg) -> std::vector<hal::TestResult> {
        return hal_stub_pass(module_id, mode, cfg);
    };

    RunOrchestrator orch(*spool, stream, hal_counting);
    orch.on_configure(make_configure("run_lt", "cpu", 2 /* LONG_TERM */));
    orch.on_start("run_lt");

    // Let it run for ~3 ticks (1 Hz), then stop.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    orch.on_stop("run_lt");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto batches = spool->fetch_unacked(20);
    ASSERT_GE(batches.size(), 2u);  // At least 2 non-terminal batches

    uint64_t prev_seq = 0;
    bool first = true;
    for (const auto& row : batches) {
        TestResultBatch batch;
        if (!batch.ParseFromArray(row.payload.data(),
                                   static_cast<int>(row.payload.size()))) continue;
        for (int i = 0; i < batch.samples_size(); ++i) {
            hal::common::TestResult pr;
            if (!batch.samples(i).payload().UnpackTo(&pr)) continue;
            if (first) {
                first = false;
                EXPECT_EQ(pr.sequence(), 0u);
            } else {
                EXPECT_GT(pr.sequence(), prev_seq);
            }
            prev_seq = pr.sequence();
        }
    }
}

// ─── ABORTED synthesis on stop ────────────────────────────────────────────────

TEST(RunOrchestrator, StopDuringLongTermProducesAborted) {
    auto spool = make_mem_spool();
    auto stream = make_stub_stream();
    RunOrchestrator orch(*spool, stream, hal_stub_pass);

    orch.on_configure(make_configure("run_ab", "cpu", 2 /* LONG_TERM */));
    orch.on_start("run_ab");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    orch.on_stop("run_ab");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto batches = spool->fetch_unacked(20);
    ASSERT_FALSE(batches.empty());

    // Find the terminal batch.
    bool found_aborted = false;
    for (const auto& row : batches) {
        TestResultBatch batch;
        if (!batch.ParseFromArray(row.payload.data(),
                                   static_cast<int>(row.payload.size()))) continue;
        if (batch.terminal()) {
            for (int i = 0; i < batch.samples_size(); ++i) {
                if (batch.samples(i).status() == hal::common::TEST_STATUS_ABORTED) {
                    found_aborted = true;
                }
            }
        }
    }
    EXPECT_TRUE(found_aborted);
}

// ─── Task 72: invalid execution mode is rejected before the run starts ────

TEST(RunOrchestrator, InvalidExecutionModeIsRejectedNotCompleted) {
    auto spool = make_mem_spool();
    auto stream = make_stub_stream();
    RunOrchestrator orch(*spool, stream, hal_stub_pass);

    // An unknown/future mode (not 1..4) must be rejected with a failed ACK and
    // a FAILED run state, never persisted as a completed run.
    ConfigureRun cfg;
    cfg.set_run_id("badmode");
    cfg.set_module_id("cpu");
    cfg.set_mode(static_cast<hal::common::ExecutionMode>(99));
    orch.on_configure(cfg);
    orch.on_start("badmode");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    orch.stop_all();

    auto batches = spool->fetch_unacked(10);
    // No result batch should be produced for a rejected run.
    EXPECT_TRUE(batches.empty());
    // Run state should reflect FAILED (not COMPLETED).
    auto states = spool->all_run_states();
    bool failed = false;
    for (const auto& rs : states) {
        if (rs.run_id == "badmode") failed = (rs.state == static_cast<int>(RunState::FAILED));
    }
    EXPECT_TRUE(failed);
}

} // namespace

// ─── Task 77: cursor-based spool replay ──────────────────────────────────────

TEST(SpoolCursor, ResumesAfterCursorWithoutDuplication) {
    auto spool = make_mem_spool();
    // Insert three batches for a backlog.
    for (int64_t i = 0; i < 3; ++i) {
        TestResultBatch b;
        b.set_run_id("run_cursor");
        b.set_batch_seq(i);
        std::string bytes; b.SerializeToString(&bytes);
        spool->insert_batch("run_cursor", i, Bytes(bytes.begin(), bytes.end()));
    }
    // First pass: fetch from cursor 0 (no rows seen yet).
    auto first = spool->fetch_unacked_after(0, 128);
    ASSERT_EQ(first.size(), 3u);
    // Advance the cursor past the first row (ins_seq — CLI-080) and fetch
    // again — must NOT re-return the first row (no duplication / no starvation
    // of later rows).
    auto after_first = spool->fetch_unacked_after(first[0].ins_seq, 128);
    ASSERT_EQ(after_first.size(), 2u);
    EXPECT_EQ(after_first[0].batch_seq, 1);
    EXPECT_EQ(after_first[1].batch_seq, 2);
    // Fully drained.
    auto after_last = spool->fetch_unacked_after(after_first[1].ins_seq, 128);
    EXPECT_TRUE(after_last.empty());
}

// ─── Task 78: disconnect ordering + dedup ────────────────────────────────────

TEST(ReplayOrdering, PreservesMonotonicSequenceAndIdempotentIdentity) {
    auto spool = make_mem_spool();
    // Simulate a backlog produced across a disconnect: three batches with
    // monotonic batch_seq (the idempotent batch identity is run_id+batch_seq).
    for (int64_t i = 0; i < 3; ++i) {
        TestResultBatch b;
        b.set_run_id("run_order");
        b.set_batch_seq(i);
        b.set_terminal(i == 2);
        std::string bytes; b.SerializeToString(&bytes);
        spool->insert_batch("run_order", i, Bytes(bytes.begin(), bytes.end()));
    }
    auto batches = spool->fetch_unacked(10);
    ASSERT_EQ(batches.size(), 3u);
    // Replay must emit batches in strictly increasing batch_seq order so the
    // server's dedup/ordering (run_id+batch_seq) sees no gaps or duplicates.
    int64_t prev = -1;
    for (const auto& row : batches) {
        TestResultBatch b;
        ASSERT_TRUE(b.ParseFromArray(row.payload.data(), static_cast<int>(row.payload.size())));
        EXPECT_GT(b.batch_seq(), prev);  // strictly increasing, no duplicate
        EXPECT_EQ(b.run_id(), "run_order");
        prev = b.batch_seq();
    }
    EXPECT_EQ(prev, 2);
}
