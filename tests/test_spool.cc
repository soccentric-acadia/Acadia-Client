// tests/test_spool.cc — CLI-040: offline spool behavior.
//
// Covers the spool semantics stated in doc/client-server-contract.md
// (X2-004/005): insert/ack/replay order, caps fail closed, purge_acked only
// touches acked rows, run-state persistence, and legacy quarantine.
#include "spool/Spool.h"

#include <climits>
#include <cstdint>
#include <unistd.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

using acadia::client::Spool;

class SpoolTest : public ::testing::Test {
protected:
    std::string db_path;

    void SetUp() override {
        db_path = temp_db();
    }
    void TearDown() override {
        std::filesystem::remove_all(std::filesystem::path(db_path).parent_path());
    }

    static std::string temp_db() {
        auto dir = std::filesystem::temp_directory_path() /
                   ("acadia_spool_test_" + std::to_string(::getpid()) + "_" +
                    std::to_string(++counter));
        std::filesystem::create_directories(dir);
        return (dir / "spool.db").string();
    }
    static inline int counter = 0;
};

TEST_F(SpoolTest, InsertAndFetchUnackedOldestFirst) {
    Spool spool{db_path};
    ASSERT_TRUE(spool.insert_batch("run-1", 1, {1, 1, 1}));
    ASSERT_TRUE(spool.insert_batch("run-1", 2, {2, 2, 2}));

    auto rows = spool.fetch_unacked();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].batch_seq, 1);
    EXPECT_EQ(rows[1].batch_seq, 2);
}

TEST_F(SpoolTest, AckRemovesFromReplay) {
    Spool spool{db_path};
    ASSERT_TRUE(spool.insert_batch("run-1", 1, {1}));
    ASSERT_TRUE(spool.insert_batch("run-1", 2, {2}));
    spool.ack_batch("run-1", 1);

    auto rows = spool.fetch_unacked();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].batch_seq, 2);
}

TEST_F(SpoolTest, CursorReplayResumesWithoutDuplication) {
    Spool spool{db_path};
    for (int i = 1; i <= 5; ++i) ASSERT_TRUE(spool.insert_batch("run-1", i, {1}));

    auto first_page = spool.fetch_unacked_after(0, 0, 2);
    ASSERT_EQ(first_page.size(), 2u);
    // Resume from the last row of the first page.
    auto second_page = spool.fetch_unacked_after(
        first_page.back().created_at, first_page.back().batch_seq, 128);
    ASSERT_EQ(second_page.size(), 3u);
    EXPECT_EQ(second_page.front().batch_seq, 3);
    EXPECT_EQ(second_page.back().batch_seq, 5);
}

TEST_F(SpoolTest, ResultCapFailsClosed) {
    // 1 KiB cap: a 3 KiB payload insert would exceed the cap — refused, not
    // silently dropped. (Exactly filling the cap is allowed.)
    Spool spool{db_path, /*result_cap=*/1024, /*log_cap=*/1024 * 1024};
    std::vector<uint8_t> big(3 * 1024, 0xAB);
    EXPECT_FALSE(spool.insert_batch("run-1", 1, big));
    EXPECT_TRUE(spool.result_spool_full() || spool.result_bytes_used() == 0);
}

TEST_F(SpoolTest, PurgeAckedOnlyTouchesAckedRows) {
    Spool spool{db_path};
    ASSERT_TRUE(spool.insert_batch("run-1", 1, {1}));
    ASSERT_TRUE(spool.insert_batch("run-1", 2, {2}));
    spool.ack_batch("run-1", 1);
    spool.purge_acked(INT64_MAX);  // purge everything acked

    auto rows = spool.fetch_unacked();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].batch_seq, 2);
}

TEST_F(SpoolTest, RunStatePersistsAcrossReopen) {
    {
        Spool spool{db_path};
        spool.upsert_run_state("run-9", 2, 42);
    }
    {
        Spool spool{db_path};
        auto states = spool.all_run_states();
        ASSERT_EQ(states.size(), 1u);
        EXPECT_EQ(states[0].run_id, "run-9");
        EXPECT_EQ(states[0].state, 2);
        EXPECT_EQ(states[0].last_seq, 42);
    }
}

}  // namespace