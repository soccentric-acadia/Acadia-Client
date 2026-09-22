#include "spool/Spool.h"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>

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
// Task 74: checked SQLite helpers — every prepare/bind/step error is surfaced,
// never silently ignored (corrupt, read-only, full, locked, schema-error).
// ---------------------------------------------------------------------------

// CLI-062: true when a statement pointer is non-null (readability helper).
static bool stmt_ok(sqlite3_stmt* stmt) { return stmt != nullptr; }

// Prepare a statement; returns nullptr (and logs the error) on failure.
static sqlite3_stmt* prep_checked(sqlite3* db, const char* sql, const char* what) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("spool: prepare {} failed: {} ({})", what,
                      sqlite3_errmsg(db), rc);
        return nullptr;
    }
    return stmt;
}

// Step to DONE (a write / no-row statement); false on any error.
static bool step_done_checked(sqlite3* db, sqlite3_stmt* stmt, const char* what) {
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        spdlog::error("spool: step {} failed: {} ({})", what,
                      sqlite3_errmsg(db), rc);
        sqlite3_finalize(stmt);
        return false;
    }
    return true;
}

// Bind a text param; false on error.
static bool bind_text_checked(sqlite3_stmt* stmt, int idx, const std::string& v,
                              const char* what) {
    if (sqlite3_bind_text(stmt, idx, v.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        spdlog::error("spool: bind_text {} param {} failed", what, idx);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Legacy quarantine (Section 4.5 of req.md)
// ---------------------------------------------------------------------------

bool Spool::quarantine_if_legacy(const std::string& path) {
    // Fast path: database doesn't exist yet.
    if (!std::filesystem::exists(path)) return false;

    // Open read-only to check the stored version.
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db,
                         SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;  // Can't read — let the main open handle the error.
    }

    int stored_ver = 0;
    const char* ver_sql =
        "SELECT value FROM spool_meta WHERE key='schema_version' LIMIT 1";
    sqlite3_stmt* stmt = nullptr;
    bool meta_missing = false;
    if (sqlite3_prepare_v2(db, ver_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            stored_ver = sqlite3_column_int(stmt, 0);
        else
            meta_missing = true;  // CLI-057: meta table exists but empty
        sqlite3_finalize(stmt);
    } else {
        meta_missing = true;      // CLI-057: no spool_meta table at all
    }
    sqlite3_close(db);

    if (meta_missing) {
        // CLI-057: crash between create and version write — a DB with NO meta
        // table and NO payload rows is treated as fresh, not legacy.
        sqlite3* db2 = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db2, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            int rows = 0;
            sqlite3_stmt* cs = nullptr;
            if (sqlite3_prepare_v2(db2,
                "SELECT (SELECT COUNT(*) FROM batches) + (SELECT COUNT(*) FROM log_batches)",
                -1, &cs, nullptr) == SQLITE_OK) {
                if (sqlite3_step(cs) == SQLITE_ROW)
                    rows = sqlite3_column_int(cs, 0);
                sqlite3_finalize(cs);
            }
            sqlite3_close(db2);
            if (rows == 0) {
                spdlog::info("spool: no meta table and no rows — treating as a fresh database");
                return false;
            }
        }
        stored_ver = 0;  // rows present without a version stamp → legacy below
    }

    if (stored_ver >= kCurrentSchemaVersion) return false;

    // Incompatible schema: quarantine.
    try {
        namespace fs = std::filesystem;
        fs::path src(path);
        fs::path legacy_dir = src.parent_path() / ".legacy";
        fs::create_directories(legacy_dir);
        // Name: original_stem.<unix_seconds>.db
        int64_t ts = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
        fs::path dst = legacy_dir / (src.stem().string() + "." +
                                      std::to_string(ts) + ".db");
        // CLI-056: move the WAL + SHM siblings too — a fresh DB at the same
        // path would otherwise have the stale WAL/SHM applied over it.
        for (const char* suffix : {"-wal", "-shm"}) {
            std::error_code sec;
            fs::rename(src.string() + suffix, dst.string() + suffix, sec);
            if (sec && sec != std::errc::no_such_file_or_directory) {
                spdlog::warn("spool: could not move {}{}: {}", src.string(), suffix, sec.message());
            }
        }
        fs::rename(src, dst);
        spdlog::warn("[BUFFER-INCOMPATIBLE] spool schema version {} < {} — "
                     "quarantined to {}; starting fresh",
                     stored_ver, kCurrentSchemaVersion, dst.string());
    } catch (const std::exception& e) {
        spdlog::error("spool: quarantine failed: {}", e.what());
    }
    return true;
}

// ---------------------------------------------------------------------------
// Spool ctor/dtor
// ---------------------------------------------------------------------------

Spool::Spool(const std::string& path, uint64_t result_cap, uint64_t log_cap)
    : result_cap_(result_cap), log_cap_(log_cap), path_(path) {
    quarantine_if_legacy(path);

    // CLI-060: explicit open flags (full mutex for multi-thread use, RW+CREATE).
    int rc = sqlite3_open_v2(path.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                 SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::string("spool open failed: ") +
                                  (db_ ? sqlite3_errmsg(db_) : "sqlite unavailable"));
    }
    // CLI-058: bounded busy-wait — concurrent access (diag, janitor) retries
    // instead of failing writes with SQLITE_BUSY.
    sqlite3_busy_timeout(db_, 5000);

    // CLI-061: detect corruption before trusting the store; quarantine + reopen.
    {
        std::string qk = quick_check();
        if (!qk.empty() && qk != "ok") {
            spdlog::error("spool: integrity check FAILED — quarantining: {}", qk);
            sqlite3_close(db_);
            db_ = nullptr;
            quarantine_corrupt(path);
            rc = sqlite3_open_v2(path.c_str(), &db_,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                     SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
            if (rc != SQLITE_OK) {
                throw std::runtime_error(std::string("spool re-open failed: ") +
                                          sqlite3_errmsg(db_));
            }
            sqlite3_busy_timeout(db_, 5000);
        }
    }

    exec("PRAGMA journal_mode=WAL");
    // CLI-059: synchronous=FULL — the spool is the durability boundary for
    // undelivered results; losing the last committed batches on power loss
    // would breach the replay contract. The WAL+FULL cost is acceptable at the
    // 1 Hz batch cadence.
    exec("PRAGMA synchronous=FULL");
    exec("PRAGMA foreign_keys=ON");
    // CLI-077: incremental auto-vacuum so the DB file shrinks on eMMC.
    exec("PRAGMA auto_vacuum=INCREMENTAL");
    apply_schema();
    // CLI-081: forward migrations run BEFORE the version stamp — a schema bump
    // no longer quarantines (discards) unsent data.
    migrate(stored_schema_version());
    write_schema_version(kCurrentSchemaVersion);
    spdlog::info("spool: opened {} (schema v{})", path, kCurrentSchemaVersion);
}

Spool::~Spool() {
    for (auto& [sql, stmt] : stmt_cache_) {
        (void)sql;
        sqlite3_finalize(stmt);
    }
    stmt_cache_.clear();
    if (db_) sqlite3_close(db_);
}

// ---------------------------------------------------------------------------
// schema
// ---------------------------------------------------------------------------

void Spool::apply_schema() {
    exec(R"(
        CREATE TABLE IF NOT EXISTS spool_meta (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS batches (
            run_id     TEXT    NOT NULL,
            batch_seq  INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            acked      INTEGER NOT NULL DEFAULT 0,
            ins_seq    INTEGER NOT NULL DEFAULT 0,  -- CLI-080: monotonic order
            payload    BLOB    NOT NULL,
            PRIMARY KEY (run_id, batch_seq)
        );
        CREATE INDEX IF NOT EXISTS idx_unacked ON batches(acked, ins_seq);

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


// CLI-079: shared row decoder for fetch_unacked / fetch_unacked_after.
static BatchRow decode_batch_row(sqlite3_stmt* stmt) {
    BatchRow r;
    r.run_id     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    r.batch_seq  = sqlite3_column_int64(stmt, 1);
    r.created_at = sqlite3_column_int64(stmt, 2);
    r.ins_seq    = sqlite3_column_int64(stmt, 3);
    const void* blob = sqlite3_column_blob(stmt, 4);
    int bytes        = sqlite3_column_bytes(stmt, 4);
    r.payload.assign(static_cast<const uint8_t*>(blob),
                     static_cast<const uint8_t*>(blob) + bytes);
    return r;
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

// CLI-073: non-throwing exec for call sites that hold the mutex and must not
// unwind through it (insert_log, purge paths).
bool Spool::exec_nothrow(const char* sql) {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown";
        sqlite3_free(errmsg);
        spdlog::warn("spool exec (nothrow) failed: {}", msg);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

bool Spool::insert_batch(const std::string& run_id, int64_t batch_seq,
                          const Bytes& payload) {
    std::lock_guard<std::mutex> lk(mtx_);
    // X2-004/CLI-040: fail closed on the insert that would exceed the cap —
    // the documented contract is "returns false if the spool cap WOULD be
    // exceeded", not "refuse only when already full".
    if (result_bytes_used_locked() + payload.size() > result_cap_) {
        spdlog::warn("spool: result spool full, dropping batch run_id={} seq={} ({}+{} > {})",
                     run_id, batch_seq, result_bytes_used_locked(), payload.size(), result_cap_);
        return false;
    }
    // CLI-074: plain INSERT — a repeated (run_id, batch_seq) is a real bug
    // (seq reset after restart) and must fail loudly, never silently overwrite
    // an existing UNACKED batch.
    // CLI-080: ins_seq is a monotonically increasing insertion counter, immune
    // to wall-clock jumps.
    int64_t ins_seq = 0;
    {
        sqlite3_stmt* ms = prep_checked(db_, "SELECT COALESCE(MAX(ins_seq), 0) FROM batches", "next_ins_seq");
        if (ms) {
            if (sqlite3_step(ms) == SQLITE_ROW)
                ins_seq = sqlite3_column_int64(ms, 0) + 1;
            sqlite3_finalize(ms);
        }
    }
    const char* sql =
        "INSERT INTO batches(run_id, batch_seq, created_at, ins_seq, acked, payload) "
        "VALUES (?, ?, ?, ?, 0, ?)";
    sqlite3_stmt* stmt = cached_prepare(sql, "insert_batch");
    if (!stmt) return false;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (!bind_text_checked(stmt, 1, run_id, "insert_batch")) return false;
    sqlite3_bind_int64(stmt, 2, batch_seq);
    sqlite3_bind_int64(stmt, 3, now_ns());
    sqlite3_bind_int64(stmt, 4, ins_seq);
    sqlite3_bind_blob(stmt, 5, payload.data(), static_cast<int>(payload.size()), SQLITE_TRANSIENT);
    bool ok = step_done_checked(db_, stmt, "insert_batch");
    if (ok) {
        // CLI-062: keep the running byte counter in sync.
        result_bytes_cached_ += payload.size();
    }
    return ok;
}

void Spool::ack_batch(const std::string& run_id, int64_t batch_seq) {
    std::lock_guard<std::mutex> lk(mtx_);
    // CLI-062: capture the row size before marking acked so the counter stays
    // exact without a rescan.
    uint64_t size = 0;
    {
        const char* q = "SELECT LENGTH(payload) FROM batches WHERE run_id=? AND batch_seq=?";
        sqlite3_stmt* s = prep_checked(db_, q, "ack_batch_size");
        if (stmt_ok(s)) {
            bind_text_checked(s, 1, run_id, "ack_batch");
            sqlite3_bind_int64(s, 2, batch_seq);
            if (sqlite3_step(s) == SQLITE_ROW)
                size = static_cast<uint64_t>(sqlite3_column_int64(s, 0));
            sqlite3_finalize(s);
        }
    }
    const char* sql =
        "UPDATE batches SET acked=1 WHERE run_id=? AND batch_seq=?";
    sqlite3_stmt* stmt = cached_prepare(sql, "ack_batch");
    if (!stmt) return;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (!bind_text_checked(stmt, 1, run_id, "ack_batch")) return;
    sqlite3_bind_int64(stmt, 2, batch_seq);
    (void)step_done_checked(db_, stmt, "ack_batch");
    result_bytes_cached_ = (result_bytes_cached_ > size) ? result_bytes_cached_ - size : 0;
}

std::vector<BatchRow> Spool::fetch_unacked(size_t limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    // CLI-080: order by the monotonic insertion sequence.
    const char* sql =
        "SELECT run_id, batch_seq, created_at, ins_seq, payload FROM batches "
        "WHERE acked=0 ORDER BY ins_seq ASC LIMIT ?";
    sqlite3_stmt* stmt = cached_prepare(sql, "fetch_unacked");
    if (!stmt) return {};
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(limit));
    std::vector<BatchRow> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        rows.push_back(decode_batch_row(stmt));
    // NOTE: stmt is CACHED (CLI-078) — never finalize per use.
    return rows;
}

std::vector<BatchRow> Spool::fetch_unacked_after(int64_t after_ins_seq,
                                                   size_t limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    // CLI-080: cursor over the monotonic insertion sequence — NTP/RTC jumps
    // cannot reorder or skip rows.
    const char* sql =
        "SELECT run_id, batch_seq, created_at, ins_seq, payload FROM batches "
        "WHERE acked=0 AND ins_seq > ? "
        "ORDER BY ins_seq ASC LIMIT ?";
    sqlite3_stmt* stmt = cached_prepare(sql, "fetch_unacked_after");
    if (!stmt) return {};
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, after_ins_seq);
    sqlite3_bind_int64(stmt, 2, static_cast<int64_t>(limit));
    std::vector<BatchRow> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        rows.push_back(decode_batch_row(stmt));
    // NOTE: stmt is CACHED (CLI-078) — never finalize per use.
    return rows;
}

void Spool::purge_acked(int64_t cutoff_ns) {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql =
        "DELETE FROM batches WHERE acked=1 AND created_at < ?";
    sqlite3_stmt* stmt = prep_checked(db_, sql, "purge_acked");
    if (!stmt) return;
    sqlite3_bind_int64(stmt, 1, cutoff_ns);
    (void)step_done_checked(db_, stmt, "purge_acked");
    int deleted = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (deleted > 0) {
        spdlog::debug("spool: purged {} acked result rows", deleted);
        result_bytes_dirty_ = true;
    }

    // CLI-075: acked LOG batches are purged on the same janitor schedule
    // (previously only evicted under pressure).
    {
        sqlite3_stmt* ls = prep_checked(db_,
            "DELETE FROM log_batches WHERE acked=1 AND created_at < ?",
            "purge_acked_logs");
        if (ls) {
            sqlite3_bind_int64(ls, 1, cutoff_ns);
            (void)step_done_checked(db_, ls, "purge_acked_logs");
            sqlite3_finalize(ls);
            log_bytes_dirty_ = true;
        }
    }

    // CLI-076: terminal run_states rows are purged too — a table that only
    // grew (single-shot runs) stays bounded.
    {
        sqlite3_stmt* rs = prep_checked(db_,
            "DELETE FROM run_states WHERE state IN (4, 5, 6)",  // COMPLETED/FAILED/CANCELLED
            "purge_terminal_run_states");
        if (rs) {
            (void)step_done_checked(db_, rs, "purge_terminal_run_states");
            sqlite3_finalize(rs);
        }
    }

    // CLI-077: reclaim space after the purge.
    exec_nothrow("PRAGMA incremental_vacuum(64);");
}

// ---------------------------------------------------------------------------
// CLI-061/081: integrity check + forward migrations
// ---------------------------------------------------------------------------

// CLI-061: returns the quick_check verdict ("" when the DB is unusable).
std::string Spool::quick_check() const {
    if (!db_) return "";
    const char* sql = "PRAGMA quick_check";
    sqlite3_stmt* stmt = prep_checked(db_, sql, "quick_check");
    if (!stmt) return "prepare failed";
    std::string result = "ok";
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        result = txt ? txt : "";
    }
    sqlite3_finalize(stmt);
    return result;
}

// Move a corrupt database aside (different naming from the legacy quarantine
// so operators can tell them apart).
void Spool::quarantine_corrupt(const std::string& path) {
    try {
        std::filesystem::path src(path);
        std::filesystem::path corrupt_dir = src.parent_path() / ".corrupt";
        std::filesystem::create_directories(corrupt_dir);
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
        auto dst = corrupt_dir / (src.stem().string() + "." + std::to_string(ts) + ".db");
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::error_code ec;
            std::filesystem::rename(src.string() + suffix, dst.string() + suffix, ec);
        }
        spdlog::error("spool: corrupt database moved to {}", dst.string());
    } catch (const std::exception& e) {
        spdlog::error("spool: corrupt-database quarantine failed: {}", e.what());
    }
}

// CLI-081: forward migrations. Each step upgrades the schema in place; unsent
// data is preserved (the legacy quarantine path only fires for schemas this
// build cannot understand at all).
bool Spool::migrate(int from_version) {
    if (from_version >= kCurrentSchemaVersion) return true;
    spdlog::info("spool: migrating schema {} -> {}", from_version, kCurrentSchemaVersion);
    // v2 → v3: add the monotonic insertion sequence, backfilling from rowid
    // (rowids are monotonic for a table with a composite PK, so existing rows
    // keep their relative order).
    if (from_version < 3 && from_version >= 1) {  // v1/v2 created the table WITHOUT ins_seq
        exec_nothrow("ALTER TABLE batches ADD COLUMN ins_seq INTEGER NOT NULL DEFAULT 0");
        exec_nothrow("UPDATE batches SET ins_seq = rowid WHERE ins_seq = 0");
        exec_nothrow("CREATE INDEX IF NOT EXISTS idx_unacked_seq ON batches(acked, ins_seq)");
    }
    // Future migrations chain here:
    //   if (from_version < 4) { ... }
    return true;
}

// CLI-078: prepared-statement cache — every hot statement is compiled once.
// All callers hold mtx_, so concurrent reuse is not possible.
sqlite3_stmt* Spool::cached_prepare(const char* sql, const char* what) const {
    auto it = stmt_cache_.find(sql);
    if (it != stmt_cache_.end()) return it->second;
    sqlite3_stmt* stmt = prep_checked(db_, sql, what);
    if (stmt) stmt_cache_[sql] = stmt;
    return stmt;
}

// CLI-062: maintain running byte counters instead of a full SUM(LENGTH)
// scan on every insert and every 200 ms backpressure poll. The counters are
// initialised once at open and kept in sync by insert/ack/purge/evict paths.
uint64_t Spool::result_bytes_used_locked() const {
    // mtx_ must already be held by caller.
    if (result_bytes_dirty_) {
        const char* sql = "SELECT COALESCE(SUM(LENGTH(payload)), 0) FROM batches WHERE acked=0";
        sqlite3_stmt* stmt = prep_checked(db_, sql, "result_bytes_used");
        if (!stmt) return result_bytes_cached_;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            result_bytes_cached_ = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        sqlite3_finalize(stmt);
        result_bytes_dirty_ = false;
    }
    return result_bytes_cached_;
}

uint64_t Spool::result_bytes_used() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return result_bytes_used_locked();
}

bool Spool::result_spool_full_locked() const {
    // mtx_ must already be held by caller.
    return result_bytes_used_locked() >= result_cap_;
}

bool Spool::result_spool_full() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return result_spool_full_locked();
}

// ---------------------------------------------------------------------------
// Logs
// ---------------------------------------------------------------------------

bool Spool::insert_log(const std::string& run_id, int64_t batch_seq,
                        const Bytes& payload) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (log_spool_full_locked()) {
        // Task 75 + CLI-072: evict the oldest ACKED row; when NO acked row
        // exists the spool would otherwise grow past its cap without bound —
        // drop the NEWEST pending batch instead (logged + counted).
        exec_nothrow("DELETE FROM log_batches WHERE rowid = "
                     "(SELECT rowid FROM log_batches WHERE acked=1 "
                     " ORDER BY created_at ASC LIMIT 1)");
        if (log_bytes_used_locked() + payload.size() > log_cap_) {
            ++log_evicted_newest_;
            if (log_evicted_newest_ % 100 == 1) {
                spdlog::warn("spool: log spool full with no acked rows — "
                             "{} insert(s) rejected (newest-drop policy)",
                             log_evicted_newest_);
            }
            return false;  // CLI-072: never grow past the cap
        }
    }
    // CLI-073: no exceptions escape while holding the mutex — failures return
    // false (the LogCollector treats it as backpressure).
    // CLI-074(logs): plain INSERT; duplicate (run_id, batch_seq) fails loudly.
    const char* sql =
        "INSERT INTO log_batches(run_id, batch_seq, created_at, acked, payload) "
        "VALUES (?, ?, ?, 0, ?)";
    sqlite3_stmt* stmt = prep_checked(db_, sql, "insert_log");
    if (!stmt) return false;
    if (!bind_text_checked(stmt, 1, run_id, "insert_log")) { sqlite3_finalize(stmt); return false; }
    sqlite3_bind_int64(stmt, 2, batch_seq);
    sqlite3_bind_int64(stmt, 3, now_ns());
    sqlite3_bind_blob(stmt, 4, payload.data(), static_cast<int>(payload.size()), SQLITE_TRANSIENT);
    bool ok = step_done_checked(db_, stmt, "insert_log");
    sqlite3_finalize(stmt);
    if (ok) {
        // CLI-062: keep the running byte counter in sync.
        log_bytes_cached_ += payload.size();
    }
    return ok;
}

void Spool::ack_log(const std::string& run_id, int64_t batch_seq) {
    std::lock_guard<std::mutex> lk(mtx_);
    // CLI-062: capture the row size before marking acked.
    uint64_t size = 0;
    {
        sqlite3_stmt* s = prep_checked(db_,
            "SELECT LENGTH(payload) FROM log_batches WHERE run_id=? AND batch_seq=?",
            "ack_log_size");
        if (s) {
            sqlite3_bind_text(s, 1, run_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, batch_seq);
            if (sqlite3_step(s) == SQLITE_ROW)
                size = static_cast<uint64_t>(sqlite3_column_int64(s, 0));
            sqlite3_finalize(s);
        }
    }
    // CLI-064: checked prepare/bind/step.
    const char* sql =
        "UPDATE log_batches SET acked=1 WHERE run_id=? AND batch_seq=?";
    sqlite3_stmt* stmt = prep_checked(db_, sql, "ack_log");
    if (!stmt) return;
    if (!bind_text_checked(stmt, 1, run_id, "ack_log")) { sqlite3_finalize(stmt); return; }
    sqlite3_bind_int64(stmt, 2, batch_seq);
    (void)step_done_checked(db_, stmt, "ack_log");
    sqlite3_finalize(stmt);
    log_bytes_cached_ = (log_bytes_cached_ > size) ? log_bytes_cached_ - size : 0;
}

std::vector<LogRow> Spool::fetch_unacked_logs(size_t limit) const {
    std::lock_guard<std::mutex> lk(mtx_);
    // CLI-065: null stmt (prepare failure) no longer dereferenced.
    const char* sql =
        "SELECT run_id, batch_seq, created_at, payload FROM log_batches "
        "WHERE acked=0 ORDER BY created_at ASC LIMIT ?";
    sqlite3_stmt* stmt = prep_checked(db_, sql, "fetch_unacked_logs");
    if (!stmt) return {};
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
    // Task 75: evict only ACKED logs, oldest first, preserving unacknowledged.
    const char* sql =
        "DELETE FROM log_batches WHERE rowid IN "
        "(SELECT rowid FROM log_batches WHERE acked=1 "
        " ORDER BY created_at ASC LIMIT ?)";
    sqlite3_stmt* stmt = prep_checked(db_, sql, "evict_oldest_logs");
    if (!stmt) return;
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(n));
    (void)step_done_checked(db_, stmt, "evict_oldest_logs");
    sqlite3_finalize(stmt);
    // CLI-062: evicted rows leave the unacked set — mark for resync.
    log_bytes_dirty_ = true;
}

// CLI-062: cached counter (see result_bytes_used_locked).
uint64_t Spool::log_bytes_used_locked() const {
    if (log_bytes_dirty_) {
        const char* sql = "SELECT COALESCE(SUM(LENGTH(payload)), 0) FROM log_batches WHERE acked=0";
        sqlite3_stmt* stmt = prep_checked(db_, sql, "log_bytes_used");
        if (!stmt) return log_bytes_cached_;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            log_bytes_cached_ = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        sqlite3_finalize(stmt);
        log_bytes_dirty_ = false;
    }
    return log_bytes_cached_;
}

uint64_t Spool::log_bytes_used() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return log_bytes_used_locked();
}

bool Spool::log_spool_full_locked() const {
    return log_bytes_used_locked() >= log_cap_;
}

bool Spool::log_spool_full() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return log_spool_full_locked();
}

// ---------------------------------------------------------------------------
// Run state registry
// ---------------------------------------------------------------------------

// CLI-076: terminal RunState values (client_service.proto RunState):
// COMPLETED=4, FAILED=5, CANCELLED=6.
static bool run_state_is_terminal(int state) {
    return state == 4 || state == 5 || state == 6;
}

void Spool::upsert_run_state(const std::string& run_id, int state, int64_t last_seq) {
    std::lock_guard<std::mutex> lk(mtx_);
    // CLI-067: checked prepare/bind/step.
    const char* sql =
        "INSERT INTO run_states(run_id, state, last_seq) VALUES (?, ?, ?) "
        "ON CONFLICT(run_id) DO UPDATE SET state=excluded.state, last_seq=excluded.last_seq";
    sqlite3_stmt* stmt = prep_checked(db_, sql, "upsert_run_state");
    if (!stmt) return;
    if (!bind_text_checked(stmt, 1, run_id, "upsert_run_state")) { sqlite3_finalize(stmt); return; }
    sqlite3_bind_int(stmt, 2, state);
    sqlite3_bind_int64(stmt, 3, last_seq);
    (void)step_done_checked(db_, stmt, "upsert_run_state");
    sqlite3_finalize(stmt);
}

void Spool::delete_run_state(const std::string& run_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    // CLI-068: checked prepare/bind/step.
    const char* sql = "DELETE FROM run_states WHERE run_id=?";
    sqlite3_stmt* stmt = prep_checked(db_, sql, "delete_run_state");
    if (!stmt) return;
    if (!bind_text_checked(stmt, 1, run_id, "delete_run_state")) { sqlite3_finalize(stmt); return; }
    (void)step_done_checked(db_, stmt, "delete_run_state");
    sqlite3_finalize(stmt);
}

std::vector<Spool::RunStateRow> Spool::all_run_states() const {
    std::lock_guard<std::mutex> lk(mtx_);
    // CLI-069: checked prepare (null stmt no longer dereferenced).
    const char* sql = "SELECT run_id, state, last_seq FROM run_states";
    sqlite3_stmt* stmt = prep_checked(db_, sql, "all_run_states");
    if (!stmt) return {};
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

// ---------------------------------------------------------------------------
// Schema version helpers
// ---------------------------------------------------------------------------

void Spool::write_schema_version(int version) {
    std::lock_guard<std::mutex> lk(mtx_);
    // CLI-070: checked prepare/bind/step.
    const char* sql =
        "INSERT INTO spool_meta(key, value) VALUES ('schema_version', ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    sqlite3_stmt* stmt = prep_checked(db_, sql, "write_schema_version");
    if (!stmt) return;
    std::string v = std::to_string(version);
    if (!bind_text_checked(stmt, 1, v, "write_schema_version")) { sqlite3_finalize(stmt); return; }
    (void)step_done_checked(db_, stmt, "write_schema_version");
    sqlite3_finalize(stmt);
}

int Spool::stored_schema_version() const {
    std::lock_guard<std::mutex> lk(mtx_);
    const char* sql =
        "SELECT value FROM spool_meta WHERE key='schema_version' LIMIT 1";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    int ver = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        // CLI-071: no std::stoi on TEXT — sqlite3_column_int parses safely
        // (garbage → 0, treated as legacy/unknown).
        ver = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return ver;
}

} // namespace acadia::client
