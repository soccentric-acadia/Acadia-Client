// test_subsystems.cc — task 79: Client subsystem tests for registry, spool,
// and adapter dispatch (previously untested alongside serializer/orchestrator).
#include <gtest/gtest.h>

#include "registry/ModuleRegistry.h"
#include "registry/AdapterRegistry.h"
#include "spool/Spool.h"

#include "client_service.pb.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#include <vector>

using namespace acadia::client;
using acadia::stream::TestResultBatch;

namespace {

// A minimal adapter for testing dispatch.
class TestAdapter : public AdapterBase {
public:
    std::string module_id() const override { return "testmod"; }
    std::vector<hal::TestResult> run(int mode, const std::vector<uint8_t>& /*config*/) override {
        hal::TestResult r;
        r.module_id = "testmod";
        r.test_id   = "testmod";
        r.name      = "testmod";
        r.output.status  = (mode >= 1 && mode <= 4) ? hal::Status::PASS : hal::Status::ERROR;
        r.output.severity = hal::Severity::INFO;
        return {r};
    }
};

std::string make_modules_yaml() {
    return "modules:\n"
           "  - id: testmod\n"
           "    name: Test Module\n"
           "    proto_package: hal.modules.testmod\n"
           "    config_type_url: type.googleapis.com/hal.modules.testmod.Config\n"
           "    result_type_url: type.googleapis.com/hal.modules.testmod.Result\n"
           "    version: 1\n"
           "    status: active\n"
           "    category: test\n"
           "    modes:\n"
           "      - SINGLE_SHOT\n";
}

std::unique_ptr<Spool> make_mem_spool() {
    auto dir = std::filesystem::temp_directory_path() /
               ("acadia_spool_test_" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    auto p = dir / "spool.db";
    return std::make_unique<Spool>(p.string(), 64 * 1024 * 1024, 16 * 1024 * 1024);
}

// ─── ModuleRegistry ──────────────────────────────────────────────────────────

TEST(ModuleRegistry, LoadsModulesFromYaml) {
    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / "acadia_modules_test.yaml";
    std::ofstream(path) << make_modules_yaml();
    ModuleRegistry reg;
    int n = reg.load(path.string());
    EXPECT_EQ(n, 1);
    auto all = reg.all();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].id, "testmod");
    std::filesystem::remove(path);
}

TEST(ModuleRegistry, MissingFileThrows) {
    ModuleRegistry reg;
    EXPECT_THROW(reg.load("/nonexistent/modules.yaml"), std::runtime_error);
}

// ─── AdapterRegistry dispatch ────────────────────────────────────────────────

TEST(AdapterRegistry, MissingAdapterReturnsErrorNotEmpty) {
    AdapterRegistry reg;
    auto res = reg.invoke("not_there", 1, {});
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0].output.status, hal::Status::ERROR);
}

TEST(AdapterRegistry, InvalidModeFromAdapterIsError) {
    AdapterRegistry reg;
    reg.register_adapter(std::make_unique<TestAdapter>());
    // Mode 99 (invalid) -> adapter returns ERROR result.
    auto res = reg.invoke("testmod", 99, {});
    ASSERT_EQ(res.size(), 1u);
    EXPECT_EQ(res[0].output.status, hal::Status::ERROR);
}

// ─── Spool edge cases ────────────────────────────────────────────────────────

TEST(Spool, InsertAndFetchRoundTrip) {
    auto spool = make_mem_spool();
    TestResultBatch b;
    b.set_run_id("r1"); b.set_batch_seq(1); b.set_terminal(true);
    std::string bytes; b.SerializeToString(&bytes);
    EXPECT_TRUE(spool->insert_batch("r1", 1, Bytes(bytes.begin(), bytes.end())));
    auto rows = spool->fetch_unacked(10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].run_id, "r1");
}

TEST(Spool, AckedRowsNotFetchedAgain) {
    auto spool = make_mem_spool();
    TestResultBatch b;
    b.set_run_id("r2"); b.set_batch_seq(1);
    std::string bytes; b.SerializeToString(&bytes);
    spool->insert_batch("r2", 1, Bytes(bytes.begin(), bytes.end()));
    spool->ack_batch("r2", 1);
    EXPECT_TRUE(spool->fetch_unacked(10).empty());
}

TEST(Spool, MalformedPayloadStillReturnsRowForInspection) {
    auto spool = make_mem_spool();
    // Insert arbitrary bytes that are NOT a valid serialized batch.
    std::vector<uint8_t> garbage = {0xde, 0xad, 0xbe, 0xef};
    EXPECT_TRUE(spool->insert_batch("r3", 1, garbage));
    auto rows = spool->fetch_unacked(10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].payload, garbage);
}

TEST(Spool, PurgeAckedRemovesOldAckedRows) {
    auto spool = make_mem_spool();
    TestResultBatch b;
    b.set_run_id("r4"); b.set_batch_seq(1);
    std::string bytes; b.SerializeToString(&bytes);
    spool->insert_batch("r4", 1, Bytes(bytes.begin(), bytes.end()));
    spool->ack_batch("r4", 1);
    spool->purge_acked(0);  // cutoff 0 -> everything acked purged
    EXPECT_TRUE(spool->fetch_unacked(10).empty());
}

} // namespace
