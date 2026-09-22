#include "log/LogCollector.h"

#include "client_service.pb.h"

#include <spdlog/spdlog.h>

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

using namespace acadia::stream;

namespace acadia::client {

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------

LogCollector::LogCollector(Spool& spool, StreamClient& stream,
                             LogCollectorConfig cfg)
    : spool_(spool), stream_(stream), cfg_(std::move(cfg)) {}

LogCollector::~LogCollector() {
    stop();
    // CLI-135: release the persistent /dev/kmsg descriptor.
    if (kmsg_fd_ >= 0) {
        ::close(kmsg_fd_);
        kmsg_fd_ = -1;
    }
}

void LogCollector::start() {
    running_.store(true);
    thread_ = std::thread([this]{ collect_loop(); });
}

void LogCollector::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void LogCollector::set_run_id(const std::string& run_id) {
    std::lock_guard<std::mutex> lk(run_id_mtx_);
    run_id_ = run_id;
}

// ---------------------------------------------------------------------------
// Collect loop
// ---------------------------------------------------------------------------

void LogCollector::collect_loop() {
    using clock = std::chrono::steady_clock;
    auto next_flush = clock::now() +
                      std::chrono::milliseconds(cfg_.flush_interval_ms);

    while (running_.load()) {
        std::this_thread::sleep_until(next_flush);
        next_flush += std::chrono::milliseconds(cfg_.flush_interval_ms);

        flush_kmsg();
        flush_syslog();
    }
}

// ---------------------------------------------------------------------------
// /dev/kmsg reader (kernel log)
// ---------------------------------------------------------------------------

// CLI-135: /dev/kmsg is opened once and kept open; the per-file-description
// read position persists across reads, so each flush only ships records that
// arrived since the previous flush. Opening per flush would rewind to the
// start of the ring buffer and re-ship the whole kernel log every second.
void LogCollector::flush_kmsg() {
    if (kmsg_fd_ < 0) {
        kmsg_fd_ = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (kmsg_fd_ < 0) return; // not available (e.g. host dev box)
        // Skip everything already in the ring buffer: start at the end so we
        // only collect records appended after the collector started.
        lseek(kmsg_fd_, 0, SEEK_END);
    }

    std::vector<std::string> lines;
    char buf[4096];
    std::string pending = std::move(kmsg_line_remainder_);
    kmsg_line_remainder_.clear();

    while (lines.size() < cfg_.batch_max_lines) {
        ssize_t n = ::read(kmsg_fd_, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) continue;
            break; // EAGAIN (empty) or EOF — nothing new
        }
        for (ssize_t i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\n') {
                if (!pending.empty()) lines.push_back(pending);
                pending.clear();
                if (lines.size() >= cfg_.batch_max_lines) break;
            } else {
                pending.push_back(c);
            }
        }
        if (lines.size() >= cfg_.batch_max_lines) break;
    }

    // Keep an unterminated tail for the next flush instead of dropping it.
    if (lines.size() >= cfg_.batch_max_lines && !pending.empty())
        kmsg_line_remainder_ = std::move(pending);

    if (!lines.empty()) {
        std::string rid;
        { std::lock_guard<std::mutex> lk(run_id_mtx_); rid = run_id_; }
        ship_batch(lines, "kernel", rid);
    }
}

// ---------------------------------------------------------------------------
// syslog (journald via /run/systemd/journal/socket or simple /var/log/syslog)
// ---------------------------------------------------------------------------

void LogCollector::flush_syslog() {
    // Try /var/log/syslog as a portable fallback.
    // A production implementation would use sd_journal_get_fd().
    static long offset = 0;
    std::ifstream in("/var/log/syslog", std::ios::in);
    if (!in.is_open()) return;

    in.seekg(offset);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line) && lines.size() < cfg_.batch_max_lines) {
        lines.push_back(std::move(line));
    }
    offset = static_cast<long>(in.tellg());

    if (!lines.empty()) {
        std::string rid;
        { std::lock_guard<std::mutex> lk(run_id_mtx_); rid = run_id_; }
        ship_batch(lines, "syslog", rid);
    }
}

// ---------------------------------------------------------------------------
// Ship a batch to spool + stream
// ---------------------------------------------------------------------------

void LogCollector::ship_batch(const std::vector<std::string>& lines,
                               const std::string& source,
                               const std::string& run_id) {
    if (spool_.log_spool_full()) {
        spool_.evict_oldest_logs(lines.size());
    }

    LogBatch batch;
    batch.set_run_id(run_id);
    batch.set_batch_seq(++batch_seq_);

    using sc = std::chrono::system_clock;
    auto now = sc::now().time_since_epoch();
    int64_t sec  = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    int64_t nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1'000'000'000;

    for (const auto& text : lines) {
        auto* l = batch.add_lines();
        l->set_run_id(run_id);
        l->set_source(source);
        l->mutable_timestamp()->set_seconds(sec);
        l->mutable_timestamp()->set_nanos(static_cast<int32_t>(nsec));
        l->set_level("info");
        l->set_text(text);
    }

    // Spool
    Bytes bytes(batch.ByteSizeLong());
    batch.SerializeToArray(bytes.data(), static_cast<int>(bytes.size()));
    spool_.insert_log(run_id, batch_seq_, bytes);

    // Ship live
    ClientMessage msg;
    *msg.mutable_log_batch() = std::move(batch);
    stream_.send(std::move(msg));
}

} // namespace acadia::client
