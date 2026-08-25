#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/result.h"

namespace acadia::client::config {

struct ClientConfig {
    // gRPC server (outbound — connects to Acadia-Server)
    std::string server_address;
    uint16_t    server_port{50051};

    // HalService listener (inbound — PySDK connects here; no DB writes)
    // Task 70: secure by default. Bind defaults to loopback (127.0.0.1); set
    // hal_service_bind = "0.0.0.0" ONLY when an authenticated transport or
    // firewall controls access. hal_service_auth_token (optional) is checked
    // by the listener to reject unauthenticated connections.
    std::string hal_service_bind{"127.0.0.1"};
    uint16_t    hal_service_port{50052};
    std::string hal_service_auth_token;  // optional bearer token for inbound calls

    // Identity — loaded or generated at startup; persisted separately.
    // client_id_path is the file where the UUID is stored; override in tests.
    std::string client_id;
    std::string client_id_path{"/var/lib/acadia/client_id"};

    // Spool
    std::string spool_path{"/var/lib/acadia/spool/spool.db"};
    uint64_t    result_spool_cap_bytes{2ULL * 1024 * 1024 * 1024};
    uint64_t    log_spool_cap_bytes{500ULL * 1024 * 1024};

    // Extra log paths to tail
    std::vector<std::string> log_paths;

    // Path to modules.yaml (read-only reference to HAL registry)
    std::string modules_yaml_path{"/etc/acadia/modules.yaml"};

    // TLS (empty = plaintext; used in a later step)
    std::string tls_ca;
    std::string tls_cert;
    std::string tls_key;

    // Parse /etc/acadia/client.conf (TOML).
    // Also loads or creates /var/lib/acadia/client_id.
    static Result<ClientConfig> load(const std::string& path);
};

} // namespace acadia::client::config
