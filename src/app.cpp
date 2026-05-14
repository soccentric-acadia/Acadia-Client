#include "app.h"

#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>

#include "client_service.pb.h"

using namespace acadia::stream;

namespace acadia::client {

// ---------------------------------------------------------------------------
// HAL invocation stub — replaced with real HAL calls when linking against
// the Acadia-HAL library in the Yocto build.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> hal_invoke_stub(const std::string& module_id,
                                              int /*mode*/,
                                              const std::vector<uint8_t>& /*cfg*/) {
    spdlog::debug("hal_stub: module_id={}", module_id);
    return {}; // Empty result until HAL library is linked
}

// ---------------------------------------------------------------------------
// App ctor / dtor
// ---------------------------------------------------------------------------

App::App(config::ClientConfig cfg) : cfg_(std::move(cfg)) {}
App::~App() = default;

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool App::init() {
    spdlog::info("app: init client_id={} server={}:{}",
        cfg_.client_id, cfg_.server_address, cfg_.server_port);

    // 1. Open spool
    try {
        spool_ = std::make_unique<Spool>(
            cfg_.spool_path,
            cfg_.result_spool_cap_bytes,
            cfg_.log_spool_cap_bytes);
    } catch (const std::exception& e) {
        spdlog::critical("app: spool init failed: {}", e.what());
        return false;
    }

    // 2. Module registry
    registry_ = std::make_unique<ModuleRegistry>();
    if (!cfg_.modules_yaml_path.empty()) {
        try {
            int n = registry_->load(cfg_.modules_yaml_path);
            spdlog::info("app: loaded {} modules", n);
        } catch (const std::exception& e) {
            spdlog::warn("app: module registry load failed: {} (continuing)", e.what());
        }
    }

    // 3. Stream client
    StreamClientConfig scfg;
    scfg.server_address = cfg_.server_address;
    scfg.server_port    = cfg_.server_port;
    scfg.tls_ca         = cfg_.tls_ca;
    scfg.tls_cert       = cfg_.tls_cert;
    scfg.tls_key        = cfg_.tls_key;

    stream_ = std::make_unique<StreamClient>(scfg,
        [this](const ServerMessage& msg){ on_server_message(msg); });

    // 4. Run orchestrator
    orchestrator_ = std::make_unique<RunOrchestrator>(
        *spool_, *stream_, hal_invoke_stub);

    // 5. Log collector
    LogCollectorConfig lccfg;
    lccfg.extra_paths = cfg_.log_paths;
    log_collector_ = std::make_unique<LogCollector>(*spool_, *stream_, lccfg);

    // 6. Janitor
    JanitorConfig jcfg;
    jcfg.board_id = cfg_.client_id;
    janitor_ = std::make_unique<Janitor>(*spool_, *stream_, jcfg);

    // 7. Diagnostic socket (T045)
    diag_socket_ = std::make_unique<DiagSocket>(
        "/run/acadia/client.sock",
        [this](const std::string& query) -> std::string {
            auto states = spool_->all_run_states();
            std::string r = R"({"connected":)";
            r += stream_->is_connected() ? "true" : "false";
            r += R"(,"spool_bytes":)" + std::to_string(spool_->result_bytes_used());
            r += R"(,"active_runs":)" + std::to_string(states.size());
            r += R"(,"client_version":")" ACADIA_CLIENT_VERSION "\"";
            r += "}";
            return r;
        });

    return true;
}

// ---------------------------------------------------------------------------
// run
// ---------------------------------------------------------------------------

void App::run(std::atomic<bool>& shutdown) {
    spdlog::info("app: starting subsystems");

    stream_->start();
    log_collector_->start();
    janitor_->start();
    try {
        diag_socket_->start();
    } catch (const std::exception& e) {
        spdlog::warn("app: diag socket failed: {} (continuing)", e.what());
    }

    // Wait for stream to connect, then send Hello
    for (int i = 0; i < 30 && !stream_->is_connected(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (stream_->is_connected()) {
        send_hello();
        send_unacked_replay();
    }

    spdlog::info("app: running — waiting for SIGTERM/SIGINT");
    while (!shutdown.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Re-send Hello + replay if we (re)connected
        static bool was_connected = false;
        bool is_now = stream_->is_connected();
        if (is_now && !was_connected) {
            send_hello();
            send_unacked_replay();
        }
        was_connected = is_now;
    }

    spdlog::info("app: shutdown — stopping subsystems");
    diag_socket_->stop();
    janitor_->stop();
    log_collector_->stop();
    orchestrator_->stop_all();
    stream_->stop();
    spdlog::info("app: stopped");
}

// ---------------------------------------------------------------------------
// on_server_message
// ---------------------------------------------------------------------------

void App::on_server_message(const ServerMessage& msg) {
    switch (msg.payload_case()) {
        case ServerMessage::kConfigureRun:
            orchestrator_->on_configure(msg.configure_run());
            break;
        case ServerMessage::kStartRun:
            orchestrator_->on_start(msg.start_run().run_id());
            log_collector_->set_run_id(msg.start_run().run_id());
            break;
        case ServerMessage::kPauseRun:
            orchestrator_->on_pause(msg.pause_run().run_id());
            break;
        case ServerMessage::kResumeRun:
            orchestrator_->on_resume(msg.resume_run().run_id());
            break;
        case ServerMessage::kStopRun:
            orchestrator_->on_stop(msg.stop_run().run_id());
            log_collector_->set_run_id("_system");
            break;
        case ServerMessage::kBatchAck:
            orchestrator_->on_batch_ack(
                msg.batch_ack().run_id(), msg.batch_ack().batch_seq());
            break;
        case ServerMessage::kLogBatchAck:
            spool_->ack_log(msg.log_batch_ack().run_id(),
                             msg.log_batch_ack().batch_seq());
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// send_hello  (T037 board registration / T048 reconnect)
// ---------------------------------------------------------------------------

void App::send_hello() {
    ClientMessage msg;
    auto* hello = msg.mutable_hello();
    hello->set_board_id(cfg_.client_id);
    hello->set_client_version(ACADIA_CLIENT_VERSION);

    // Attach active run states from spool
    for (const auto& rs : spool_->all_run_states()) {
        auto* ai = hello->add_runs();
        ai->set_run_id(rs.run_id);
        ai->set_state(static_cast<acadia::stream::RunState>(rs.state));
        ai->set_last_batch_seq(rs.last_seq);
    }

    stream_->send(std::move(msg));
    spdlog::info("app: hello sent with {} active runs",
                 spool_->all_run_states().size());
}

// ---------------------------------------------------------------------------
// send_unacked_replay  (T048 reconnect replay)
// ---------------------------------------------------------------------------

void App::send_unacked_replay() {
    auto batches = spool_->fetch_unacked(128);
    if (batches.empty()) return;
    spdlog::info("app: replaying {} unacked batches", batches.size());
    for (const auto& row : batches) {
        TestResultBatch batch;
        if (!batch.ParseFromArray(row.payload.data(),
                                   static_cast<int>(row.payload.size()))) {
            spdlog::warn("app: replay: failed to parse batch run_id={} seq={}",
                         row.run_id, row.batch_seq);
            continue;
        }
        ClientMessage msg;
        *msg.mutable_result_batch() = std::move(batch);
        stream_.get()->send(std::move(msg));
    }
}

} // namespace acadia::client

