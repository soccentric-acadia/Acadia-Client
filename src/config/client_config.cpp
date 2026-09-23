#include "config/client_config.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "config/client_id.h"

#include <toml++/toml.hpp>
#include <spdlog/spdlog.h>

namespace acadia::client::config {

namespace {
static constexpr const char* kDefaultClientIdPath = "/var/lib/acadia/client_id";
} // namespace

Result<ClientConfig> ClientConfig::load(const std::string& path) {
    // CLI-050: a missing file throws a different path than a parse error —
    // report "file not found" clearly.
    {
        std::ifstream probe(path);
        if (!probe) {
            return Result<ClientConfig>::err("config file not found: " + path +
                                             " (" + std::strerror(errno) + ")");
        }
    }
    toml::table tbl;
    try {
        tbl = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        return Result<ClientConfig>::err(std::string("TOML parse error: ") + e.what());
    }

    ClientConfig cfg;

    // Required
    auto addr = tbl["server_address"].value<std::string>();
    if (!addr) {
        return Result<ClientConfig>::err("missing required field: server_address");
    }
    cfg.server_address = *addr;

    // Optional with defaults
    if (auto v = tbl["server_port"].value<int64_t>()) {
        if (*v < 1 || *v > 65535) {
            return Result<ClientConfig>::err("server_port out of range");
        }
        cfg.server_port = static_cast<uint16_t>(*v);
    }

    if (auto v = tbl["hal_service_port"].value<int64_t>()) {
        if (*v < 1 || *v > 65535) {
            return Result<ClientConfig>::err("hal_service_port out of range");
        }
        cfg.hal_service_port = static_cast<uint16_t>(*v);
    }
    if (auto v = tbl["hal_service_bind"].value<std::string>()) { cfg.hal_service_bind = *v; }
    if (auto v = tbl["hal_service_auth_token"].value<std::string>()) { cfg.hal_service_auth_token = *v; }

    // X2-003: stream token from conf, overridable via ACADIA_SERVER_TOKEN env.
    if (auto v = tbl["server_stream_token"].value<std::string>()) { cfg.server_stream_token = *v; }
    // CLI-049: consistent ACADIA_* env overrides for all secrets.
    if (const char* env = std::getenv("ACADIA_SERVER_TOKEN"); env && *env) {
        cfg.server_stream_token = env;
    }
    if (const char* env = std::getenv("ACADIA_HAL_AUTH_TOKEN"); env && *env) {
        cfg.hal_service_auth_token = env;
    }

    // CLI-049: consistent ACADIA_* env overrides for secret-ish values.
    if (auto v = tbl["spool_path"].value<std::string>())         { cfg.spool_path      = *v; }
    if (auto v = tbl["client_id_path"].value<std::string>())     { cfg.client_id_path  = *v; }
    if (auto v = tbl["modules_yaml_path"].value<std::string>())  { cfg.modules_yaml_path = *v; }
    if (auto v = tbl["tls_ca"].value<std::string>())             { cfg.tls_ca          = *v; }
    if (auto v = tbl["tls_cert"].value<std::string>())           { cfg.tls_cert        = *v; }
    if (auto v = tbl["tls_key"].value<std::string>())            { cfg.tls_key         = *v; }

    // CLI-047: mTLS is both-or-neither.
    if (cfg.tls_cert.empty() != cfg.tls_key.empty()) {
        return Result<ClientConfig>::err(
            "tls_cert and tls_key must be configured together (half-configured mTLS)");
    }

    if (auto v = tbl["insecure"].value<bool>())                { cfg.insecure = *v; }
    if (auto v = tbl["log_level"].value<std::string>())        { cfg.log_level = *v; }
    if (auto v = tbl["diag_socket_path"].value<std::string>()) { cfg.diag_socket_path = *v; }

    // CLI-044/045: negative values must not wrap into huge uint64 sizes.
    if (auto v = tbl["result_spool_cap_bytes"].value<int64_t>()) {
        if (*v <= 0) return Result<ClientConfig>::err("result_spool_cap_bytes must be > 0");
        cfg.result_spool_cap_bytes = static_cast<uint64_t>(*v);
    }
    if (auto v = tbl["log_spool_cap_bytes"].value<int64_t>()) {
        if (*v <= 0) return Result<ClientConfig>::err("log_spool_cap_bytes must be > 0");
        cfg.log_spool_cap_bytes = static_cast<uint64_t>(*v);
    }

    if (const toml::array* arr = tbl["log_paths"].as_array()) {
        for (const auto& elem : *arr) {
            if (auto s = elem.value<std::string>()) {
                cfg.log_paths.push_back(std::move(*s));
            }
        }
    }

    // CLI-048: refuse a non-loopback HalService bind without an auth token —
    // unauthenticated HAL RPC must never listen on the network.
    if (cfg.hal_service_bind != "127.0.0.1" && cfg.hal_service_bind != "localhost" &&
        cfg.hal_service_auth_token.empty()) {
        return Result<ClientConfig>::err(
            "hal_service_auth_token is required when hal_service_bind is not loopback "
            "(unauthenticated HAL RPC must not listen on the network)");
    }

    // CLI-046: warn on unknown top-level keys (catches typos).
    {
        static const char* kKnownKeys[] = {
            "server_address", "server_port", "server_stream_token",
            "hal_service_bind", "hal_service_port", "hal_service_auth_token",
            "client_id_path", "spool_path", "result_spool_cap_bytes",
            "log_spool_cap_bytes", "modules_yaml_path", "tls_ca", "tls_cert",
            "tls_key", "insecure", "log_level", "diag_socket_path", "log_paths",
        };
        for (const auto& [key, val] : tbl) {
            (void)val;
            bool known = false;
            for (const char* k : kKnownKeys) {
                if (key == k) { known = true; break; }
            }
            if (!known) {
                spdlog::warn("config: unknown key '{}' in {} (typo?)", key.str(), path);
            }
        }
    }

    // Load (or create) the persistent client identity.
    // Path defaults to kDefaultClientIdPath but can be overridden in client.conf
    // (useful for running multiple instances or in integration tests).
    if (cfg.client_id_path.empty()) {
        cfg.client_id_path = kDefaultClientIdPath;
    }
    auto id = load_or_create_client_id(cfg.client_id_path);
    if (!id) {
        return Result<ClientConfig>::err("client_id: " + id.error());
    }
    cfg.client_id = *id;

    spdlog::debug("config loaded: server={}:{} spool={} client_id={}",
        cfg.server_address, cfg.server_port, cfg.spool_path, cfg.client_id);

    return Result<ClientConfig>::ok(std::move(cfg));
}

} // namespace acadia::client::config
