#include "spool/Spool.h"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <stdexcept>
#include <cstring>

namespace acadia::client {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ---------------------------------------------------------------------------
// Spool ctor/dtor
// ---------------------------------------------------------------------------

Spool::Spool(const std::string& path, uint64_t result_cap, uint64_t log_cap)
    : result_cap_(result_cap), log_cap_(log_cap) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::string("spool open failed: ") +
                                  sqlite3_errmsg(db_));
    }
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec("PRAGMA foreign_keys=ON");
    apply_schema();
    spdlog::info("spool: opened {}", path);
}

Spool::~Spool() {
    if (db_) sqlite3_close(db_);
}

// ---------------------------------------------------------------------------
// schema
// ---------------------------------------------------------------------------

void Spool::apply_schema() {
    exec(R"(
        CREATE TABLE IF NOT EXISTS batches (
            run_id     TEXT    NOT NULL,
            batch_seq  INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            acked      INTEGER NOT NULL DEFAULT 0,
            payload    BLOB    NOT NULL,
            PRIMARY KEY (run_id, batch_seq)
        );
        CREATE INDEX IF NOT EXISTS idx_unacked ON batches(acked, created_at);

        CREATE TABLE IF NOT EXISTS log_batches (
            run_id     TEXT    NOT NULL,
            batch_seq  INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            acked      INTEGER NOT NULL DEFAULT 0,
            payload    BLOB    NOT NULL,
            PRIMARY KEY (run_id, batch_seq)
        );
        CREATE INDEX IF NOT EXISTS idx_log_unacked ON log_batches(acked, created_at);

        CREATE TABLE IF NOT EXISTS run_states (
            run_id   TEXT    PRIMARY KEY,
            state    INTEGER NOT NULL DEFAULT 0,
            last_seq INTEGER NOT NULL DEFAULT 0
        );
    )");
}

// ---------------------------------------------------------------------------
// exec (no-bind helper for DDL and pragmas)
// ---------------------------------------------------------------------------

void Spool::exec(const char* sql) {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown";
        sqlite3_free(errmsg);
        throw std::runtime_error("spool exec failed: " + msg);
    }
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

bool Spool::insert_batch(const std::string& run_id, int64_t batch_seq,
                          const Bytes& payload) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (result_spool_full()) {
        spdlog::warn("spool: result spool full, dropping batch run_id={} seq={}",
                     run_id, batch_seq);
        return false;
    }
    const char* sql =
        "INSERT OR REPLACE INTO batches(run_id, batch_seq, created_at, acked, payload) "
        "VALUES (?, ?, ?, 0, ?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, batch_seq);
    sqlite3_bind_int64(stmt, 3, now_ns());
    sqlite3_bind_blob(stmt, 4, payload.data(), static_cast<int>(payload.size()), SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

void Spool::ack_batch(const std::string& run_id, int64_t batch_seq) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql =
        "UPDATE batches SET acked=1 WHERE run_id=? AND batch_seq=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, batch_seq);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<BatchRow> Spool::fetch_unacked(size_t limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql =
        "SELECT run_id, batch_seq, created_at, payload FROM batches "
        "WHERE acked=0 ORDER BY created_at ASC LIMIT ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(limit));
    std::vector<BatchRow> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BatchRow r;
        r.run_id     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        r.batch_seq  = sqlite3_column_int64(stmt, 1);
        r.created_at = sqlite3_column_int64(stmt, 2);
        const void* blob = sqlite3_column_blob(stmt, 3);
        int bytes        = sqlite3_column_bytes(stmt, 3);
        r.payload.assign(static_cast<const uint8_t*>(blob),
                         static_cast<const uint8_t*>(blob) + bytes);
        rows.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return rows;
}

void Spool::purge_acked(int64_t cutoff_ns) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql =
        "DELETE FROM batches WHERE acked=1 AND created_at < ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, cutoff_ns);
    sqlite3_step(stmt);
    int deleted = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (deleted > 0) spdlog::debug("spool: purged {} acked result rows", deleted);
}

uint64_t Spool::result_bytes_used() const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "SELECT COALESCE(SUM(LENGTH(payload)), 0) FROM batches WHERE acked=0";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    uint64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
    return total;
}

bool Spool::result_spool_full() const {
    // Caller must hold mtx_
    return result_bytes_used() >= result_cap_;
}

// ---------------------------------------------------------------------------
// Logs
// ---------------------------------------------------------------------------

bool Spool::insert_log(const std::string& run_id, int64_t batch_seq,
                        const Bytes& payload) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (log_spool_full()) {
        // Drop oldest acked row
        exec("DELETE FROM log_batches WHERE rowid = "
             "(SELECT rowid FROM log_batches ORDER BY created_at ASC LIMIT 1)");
    }
    const char* sql =
        "INSERT OR REPLACE INTO log_batches(run_id, batch_seq, created_at, acked, payload) "
        "VALUES (?, ?, ?, 0, ?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, batch_seq);
    sqlite3_bind_int64(stmt, 3, now_ns());
    sqlite3_bind_blob(stmt, 4, payload.data(), static_cast<int>(payload.size()), SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

void Spool::ack_log(const std::string& run_id, int64_t batch_seq) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql =
        "UPDATE log_batches SET acked=1 WHERE run_id=? AND batch_seq=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, batch_seq);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<LogRow> Spool::fetch_unacked_logs(size_t limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql =
        "SELECT run_id, batch_seq, created_at, payload FROM log_batches "
        "WHERE acked=0 ORDER BY created_at ASC LIMIT ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(limit));
    std::vector<LogRow> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        LogRow r;
        r.run_id     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        r.batch_seq  = sqlite3_column_int64(stmt, 1);
        r.created_at = sqlite3_column_int64(stmt, 2);
        const void* blob = sqlite3_column_blob(stmt, 3);
        int bytes        = sqlite3_column_bytes(stmt, 3);
        r.payload.assign(static_cast<const uint8_t*>(blob),
                         static_cast<const uint8_t*>(blob) + bytes);
        rows.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return rows;
}

void Spool::evict_oldest_logs(size_t n) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql =
        "DELETE FROM log_batches WHERE rowid IN "
        "(SELECT rowid FROM log_batches ORDER BY created_at ASC LIMIT ?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(n));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

uint64_t Spool::log_bytes_used() const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "SELECT COALESCE(SUM(LENGTH(payload)), 0) FROM log_batches WHERE acked=0";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    uint64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
    return total;
}

bool Spool::log_spool_full() const {
    return log_bytes_used() >= log_cap_;
}

// ---------------------------------------------------------------------------
// Run state registry
// ---------------------------------------------------------------------------

void Spool::upsert_run_state(const std::string& run_id, int state, int64_t last_seq) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql =
        "INSERT INTO run_states(run_id, state, last_seq) VALUES (?, ?, ?) "
        "ON CONFLICT(run_id) DO UPDATE SET state=excluded.state, last_seq=excluded.last_seq";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, state);
    sqlite3_bind_int64(stmt, 3, last_seq);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void Spool::delete_run_state(const std::string& run_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "DELETE FROM run_states WHERE run_id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<Spool::RunStateRow> Spool::all_run_states() const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql = "SELECT run_id, state, last_seq FROM run_states";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    std::vector<RunStateRow> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RunStateRow r;
        r.run_id   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        r.state    = sqlite3_column_int(stmt, 1);
        r.last_seq = sqlite3_column_int64(stmt, 2);
        rows.push_back(r);
    }
    sqlite3_finalize(stmt);
    return rows;
}

} // namespace acadia::client
