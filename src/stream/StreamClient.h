#pragma once
// stream/StreamClient.h — bidirectional gRPC stream to the Acadia Server.
//
// Lifecycle:
//   StreamClient is constructed with config + a message callback.
//   Call start() to spawn the stream thread (connects with backoff).
//   Call send() to enqueue outbound messages (bounded queue, CLI-028).
//   Reconnect is automatic on error — exponential backoff with jitter
//   (CLI-032/033), reset to 1 s after a successful session.
//   Call stop() to shut down gracefully (TryCancel, CLI-034).
//
// TLS (CLI-029/030): tls_ca/tls_cert/tls_key may be PEM CONTENTS or file
// PATHS — file contents are loaded transparently. Plaintext is only used
// when `insecure = true` is set explicitly (CLI-029).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// Forward declarations for generated proto types.
// The actual header is included in StreamClient.cc.
namespace acadia { namespace stream {
    class ClientMessage;
    class ServerMessage;
}}  // namespace acadia::stream

namespace acadia::client {

// Callback invoked on every incoming ServerMessage.
using ServerMessageCb = std::function<void(const acadia::stream::ServerMessage&)>;

struct StreamClientConfig {
    std::string server_address;
    uint16_t    server_port{50051};
    // CLI-029/030: TLS inputs may be PEM contents or file paths (both are
    // resolved); plaintext requires the explicit `insecure = true` flag.
    std::string tls_ca;
    std::string tls_cert;
    std::string tls_key;
    bool        insecure{false};   // CLI-029: explicit plaintext opt-in
    // X2-003: sent as `x-acadia-stream-token` metadata on every Connect call
    // when non-empty; must match the Server's ACADIA_STREAM_TOKEN.
    std::string stream_token;
    std::chrono::seconds max_backoff{30};
    // CLI-028: outbound queue cap (oldest dropped beyond it, with a warning).
    size_t      send_queue_cap{4096};
};

class StreamClient {
public:
    explicit StreamClient(StreamClientConfig cfg, ServerMessageCb on_message);
    ~StreamClient();

    StreamClient(const StreamClient&)            = delete;
    StreamClient& operator=(const StreamClient&) = delete;

    // Start the stream thread. Non-blocking.
    void start();

    // Gracefully stop. Blocks until the stream thread exits (CLI-034: the
    // in-flight RPC context is TryCancel'd so Finish() cannot hang).
    void stop();

    // Enqueue a message for sending.  Thread-safe.
    // Queues even while disconnected (CLI-027): configure-acks, terminal
    // batches and error batches are drained on the next (re)connect — the App
    // layer re-sends Hello first after any reconnect (CLI-038).
    // Returns false only when the bounded queue is full (oldest messages were
    // dropped — callers that need durability rely on the spool replay).
    bool send(acadia::stream::ClientMessage msg);

    bool is_connected() const noexcept;

    // CLI-041: stream metrics exposed via the diag socket.
    uint64_t reconnects() const noexcept { return reconnects_.load(); }
    size_t   send_queue_depth() noexcept;
    uint64_t dropped_messages() const noexcept { return dropped_.load(); }
    uint64_t bytes_sent() const noexcept { return bytes_sent_.load(); }

private:
    void stream_loop();
    bool run_stream();

    StreamClientConfig         cfg_;
    ServerMessageCb            on_message_;
    std::atomic<bool>          running_{false};
    std::atomic<bool>          connected_{false};
    std::thread                thread_;
    std::thread                reader_thread_;  // CLI-036: scoped reader

    // CLI-028/037: bounded queue + condition variable (no 100 Hz polling).
    std::mutex                send_queue_mtx_;
    std::condition_variable   send_queue_cv_;
    std::queue<acadia::stream::ClientMessage> send_queue_;
    size_t                    queue_cap_ = 4096;
    std::atomic<uint64_t>     dropped_{0};

    // CLI-034: the active ClientContext, TryCancel'd by stop().
    std::mutex                ctx_mu_;
    void*                     active_ctx_ = nullptr;  // grpc::ClientContext*

    // CLI-041: metrics.
    std::atomic<uint64_t>     reconnects_{0};
    std::atomic<uint64_t>     bytes_sent_{0};

    // CLI-036: join guard so an early return/exception cannot strand the
    // reader thread.
    struct ScopedJoin {
        std::thread* t = nullptr;
        ~ScopedJoin() { if (t && t->joinable()) t->join(); }
    };
};

} // namespace acadia::client