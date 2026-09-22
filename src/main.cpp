#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef SD_NOTIFY
#include <systemd/sd-daemon.h>
#endif

#include <spdlog/spdlog.h>

#include "app.h"
#include "config/client_config.h"

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

namespace {
// Written only from the signal handler; read from the main loop.
// Relaxed ordering is safe here: we just need eventual visibility.
std::atomic<bool> g_shutdown{false};

void on_signal(int sig) {
    (void)sig;
    g_shutdown.store(true, std::memory_order_relaxed);
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
    // CLI-005: use sigaction for SIGPIPE too — consistent handler installation
    // and no signal() legacy semantics (Linux signal() has BSD semantics, but
    // mixing the two APIs is fragile).
    struct sigaction ignore_sa{};
    ignore_sa.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sa.sa_mask);
    ignore_sa.sa_flags = 0;
    // gRPC connections drop without warning and we handle reconnection
    // ourselves; a broken-pipe crash would be wrong.
    sigaction(SIGPIPE, &ignore_sa, nullptr);
    // CLI-006: SIGHUP — journald rotates; the config file is only read at
    // startup, so an explicit ignore (not the default terminate) is correct.
    sigaction(SIGHUP, &ignore_sa, nullptr);
}
} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Initialise spdlog — default logger goes to stdout, which systemd/journald
    // captures from the service unit. Pattern includes timestamp + level.
    spdlog::set_pattern("[%Y-%m-%dT%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(spdlog::level::info);

    // CLI-001/002/003: iterate i < argc (a trailing --debug must not be
    // skipped), keep parsing after --config (so --config X --debug works),
    // and reject unknown flags with a real --help/--version.
    const char* config_path = "/etc/acadia/client.conf";
    bool debug_requested = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" || arg == "-c") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "acadia-client: %s requires a path argument\n", arg.c_str());
                return 2;
            }
            config_path = argv[++i];
            continue;  // CLI-002: keep parsing (no break)
        }
        if (arg == "--debug") {
            debug_requested = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            std::printf(
                "Usage: acadia-client [OPTIONS]\n"
                "\n"
                "  -c, --config PATH   configuration file (default /etc/acadia/client.conf)\n"
                "      --debug         enable debug logging\n"
                "  -h, --help          show this help\n"
                "  -V, --version       print version and exit\n");
            return 0;
        }
        if (arg == "--version" || arg == "-V") {
            std::printf("acadia-client %s\n", ACADIA_CLIENT_VERSION);
            return 0;
        }
        std::fprintf(stderr, "acadia-client: unknown option '%s' (try --help)\n", arg.c_str());
        return 2;
    }

    spdlog::info("acadia-client v{} starting, config={}",
        ACADIA_CLIENT_VERSION, config_path);

    install_signal_handlers();

    auto cfg = acadia::client::config::ClientConfig::load(config_path);
    if (!cfg) {
        spdlog::critical("failed to load config: {}", cfg.error());
        return 1;
    }

    // CLI-004: log_level from client.conf. --debug on the command line wins.
    if (debug_requested) {
        spdlog::set_level(spdlog::level::debug);
    } else if (!cfg->log_level.empty()) {
        std::string lvl = cfg->log_level;
        for (auto& ch : lvl) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lvl == "debug")        spdlog::set_level(spdlog::level::debug);
        else if (lvl == "info")    spdlog::set_level(spdlog::level::info);
        else if (lvl == "warn" || lvl == "warning") spdlog::set_level(spdlog::level::warn);
        else if (lvl == "error")   spdlog::set_level(spdlog::level::err);
        else {
            spdlog::warn("unknown log_level '{}' — using info", lvl);
        }
    }

    acadia::client::App app(std::move(*cfg));
    if (!app.init()) {
        spdlog::critical("app init failed");
        return 1;
    }

#ifdef SD_NOTIFY
    // CLI-007: Type=notify support — tell systemd we are ready and answer the
    // watchdog (WATCHDOG_SEC) every 10 s while running. A no-op when built
    // without libsystemd (SD_NOTIFY undefined).
    sd_notify(0, "READY=1");
#endif

    std::thread watchdog_ping([&] {
#ifdef SD_NOTIFY
        while (!g_shutdown.load()) {
            sd_notify(0, "WATCHDOG=1");
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
#endif
    });

    app.run(g_shutdown);

    g_shutdown.store(true);
    if (watchdog_ping.joinable()) watchdog_ping.join();

    spdlog::info("acadia-client exiting cleanly");
    return 0;
}
