#include "stream/StreamClient.h"

#include "client_service.grpc.pb.h"
#include "client_service.pb.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>

namespace acadia::client {

using namespace acadia::stream;

namespace {

// CLI-030: config values may carry PEM CONTENTS or a PATH to a PEM file.
// Heuristic: anything starting with "-----" is treated as PEM contents.
std::string resolve_pem(const std::string& value) {
    if (value.empty()) return {};
    static constexpr std::string_view kPemBegin = "-----BEGIN";
    if (value.rfind(kPemBegin, 0) == 0) return value;
    std::ifstream in(value, std::ios::binary);
    if (!in) {
        spdlog::error("stream: TLS input is neither PEM contents nor a readable file: {}", value);
        return {};
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content;
}

} // namespace

StreamClient::StreamClient(StreamClientConfig cfg, ServerMessageCb on_message)
    : cfg_(std::move(cfg)), on_message_(std::move(on_message)) {
    queue_cap_ = cfg_.send_queue_cap;
}

StreamClient::~StreamClient() {
    stop();
}

void StreamClient::start() {
    running_.store(true);
    thread_ = std::thread([this]{ stream_loop(); });
}

void StreamClient::stop() {
    running_.store(false);
    {
        // CLI-034: TryCancel the in-flight RPC so Finish() cannot block
        // forever waiting for the server to close the stream.
        std::lock_guard<std::mutex> lk(ctx_mu_);
        if (auto* ctx = static_cast<grpc::ClientContext*>(active_ctx_)) {
            ctx->TryCancel();
        }
    }
    // CLI-037: wake the writer so it observes running_ == false promptly.
    send_queue_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    connected_.store(false);
}

bool StreamClient::is_connected() const noexcept {
    return connected_.load();
}

size_t StreamClient::send_queue_depth() noexcept {
    std::lock_guard<std::mutex> lk(send_queue_mtx_);
    return send_queue_.size();
}

// CLI-027: send() queues even while disconnected — configure-acks, terminal
// batches and error batches are drained on the next (re)connect. Dropped
// messages (full queue) are counted; durable callers rely on the spool replay.
bool StreamClient::send(ClientMessage msg) {
    {
        std::lock_guard<std::mutex> lk(send_queue_mtx_);
        // CLI-028: bounded queue — drop the OLDEST message when full.
        while (send_queue_.size() >= queue_cap_) {
            send_queue_.pop();
            ++dropped_;
            if (dropped_ % 100 == 1) {
                spdlog::warn("stream: send queue full — {} oldest message(s) dropped "
                             "(durable senders must use the spool replay)",
                             dropped_);
            }
        }
        send_queue_.push(std::move(msg));
    }
    // CLI-037: wake the writer.
    send_queue_cv_.notify_one();
    return true;
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

void StreamClient::stream_loop() {
    std::chrono::seconds backoff{1};
    std::mt19937 rng{std::random_device{}()};

    while (running_.load()) {
        // CLI-032/033: jittered exponential backoff; reset to 1 s after a
        // session that actually exchanged data (run_stream returns true).
        bool session_ok = false;
        {
            session_ok = run_stream();
        }
        if (!running_.load()) break;
        connected_.store(false);
        if (session_ok) {
            // CLI-032: reset after a successful session.
            backoff = std::chrono::seconds{1};
            continue;  // reconnect immediately after a clean session
        }
        // CLI-033: full jitter over [1, backoff] to avoid a thundering herd.
        const int span = std::max(1, static_cast<int>(backoff.count()));
        auto jitter = std::chrono::seconds{
            std::uniform_int_distribution<int>(1, span)(rng)};
        ++reconnects_;
        spdlog::warn("stream: disconnected, retry in {}s (reconnects={})",
                     jitter.count(), reconnects_.load());
        std::this_thread::sleep_for(jitter);
        backoff = std::min(backoff * 2, cfg_.max_backoff);
    }
}

bool StreamClient::run_stream() {
    const std::string target =
        cfg_.server_address + ":" + std::to_string(cfg_.server_port);

    // CLI-029: plaintext requires the explicit insecure opt-in. TLS inputs
    // accept PEM contents or file paths (CLI-030).
    std::shared_ptr<grpc::ChannelCredentials> creds;
    if (cfg_.insecure) {
        creds = grpc::InsecureChannelCredentials();
        spdlog::warn("stream: INSECURE plaintext connection explicitly enabled (dev/bench only)");
    } else if (!cfg_.tls_ca.empty() || !cfg_.tls_cert.empty()) {
        grpc::SslCredentialsOptions ssl;
        ssl.pem_root_certs  = resolve_pem(cfg_.tls_ca);
        ssl.pem_cert_chain  = resolve_pem(cfg_.tls_cert);
        ssl.pem_private_key = resolve_pem(cfg_.tls_key);
        if (ssl.pem_root_certs.empty()) {
            spdlog::error("stream: TLS configured but tls_ca resolves to nothing — refusing plaintext fallback");
            return false;
        }
        creds = grpc::SslCredentials(ssl);
    } else {
        spdlog::error("stream: no TLS material and insecure is not enabled — refusing plaintext (set insecure = true explicitly for dev/bench)");
        return false;
    }

    // CLI-035: keepalive channel args — dead TCP sessions are detected quickly.
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 20000);      // 20 s pings
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);   // 10 s to respond
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    auto channel = grpc::CreateCustomChannel(target, creds, args);

    auto stub    = ClientService::NewStub(channel);

    grpc::ClientContext ctx;
    {
        // CLI-034: publish the context so stop() can TryCancel it.
        std::lock_guard<std::mutex> lk(ctx_mu_);
        active_ctx_ = &ctx;
    }
    // X2-003: present the shared stream token when configured. The server
    // validates this metadata before any state is touched.
    if (!cfg_.stream_token.empty()) {
        ctx.AddMetadata("x-acadia-stream-token", cfg_.stream_token);
    }
    auto stream = stub->Connect(&ctx);

    if (!stream) {
        spdlog::error("stream: failed to open bidi stream to {}", target);
        {
            std::lock_guard<std::mutex> lk(ctx_mu_);
            active_ctx_ = nullptr;
        }
        return false;
    }

    // CLI-036: scoped join guard — an early return cannot strand the reader.
    StreamClient::ScopedJoin reader_join;
    reader_join.t = &reader_thread_;

    bool session_ok = false;

    // CLI-031: connected_ only after the first successful Write (the stream
    // accepted a message) or first Read — lazy gRPC connect means the channel
    // object alone proves nothing.
    std::atomic<bool> ever_read{false};

    // Reader thread
    reader_thread_ = std::thread([&]{
        ServerMessage msg;
        while (stream->Read(&msg)) {
            ever_read.store(true);
            try {
                on_message_(msg);
            } catch (const std::exception& e) {
                spdlog::error("stream: on_message exception: {}", e.what());
            }
        }
    });

    // CLI-040: structured loop (no goto) — write_fail flag breaks the loop.
    bool write_fail = false;
    while (running_.load() && !write_fail) {
        std::vector<ClientMessage> batch;
        {
            std::unique_lock<std::mutex> lk(send_queue_mtx_);
            // CLI-037: wait on the CV (bounded to 100 ms so shutdown latency
            // stays small even without sends).
            send_queue_cv_.wait_for(lk, std::chrono::milliseconds(100), [&]{
                return !send_queue_.empty() || !running_.load();
            });
            while (!send_queue_.empty()) {
                batch.push_back(std::move(send_queue_.front()));
                send_queue_.pop();
            }
        }
        for (auto& m : batch) {
            if (!stream->Write(m)) {
                spdlog::warn("stream: Write failed");
                write_fail = true;
                break;
            }
            bytes_sent_ += static_cast<uint64_t>(m.ByteSizeLong());
            // CLI-031: first successful Write proves the stream accepts
            // traffic; only then report connected.
            connected_.store(true);
        }
        if (!batch.empty()) {
            session_ok = true;  // CLI-032: had at least one working write
        }
    }
    {
        std::lock_guard<std::mutex> lk(ctx_mu_);
        active_ctx_ = nullptr;
    }
    stream->WritesDone();
    auto status = stream->Finish();
    if (!status.ok()) {
        spdlog::warn("stream: finished with error {}: {}",
                     static_cast<int>(status.error_code()),
                     status.error_message());
    }
    if (reader_thread_.joinable()) reader_thread_.join();  // guard also joins
    connected_.store(false);
    return session_ok || ever_read.load();
}

} // namespace acadia::client