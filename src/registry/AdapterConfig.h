#pragma once
// registry/AdapterConfig.h — shared configuration decode + test selection for
// HAL module adapters (CLI-160 / CLI-161).
//
// Every adapter previously did `(void)config_bytes` and ran ALL available
// tests every tick. This helper:
//   1. decodes the serialized google.protobuf.Any module Config
//      (hal.modules.<id>.Config) generically via the generated descriptor
//      pool + reflection (no per-module headers needed in the adapters);
//   2. exposes the normalized selection + common timing parameters;
//   3. selects tests honouring the configured list, and gates destructive
//      tests (watchdog/reset/power/storage-write families) so they are never
//      part of an implicit "run everything" loop.

#include "lib_base.h"

#include <cstdint>
#include <string>
#include <vector>

namespace acadia::client {

// Normalized per-run configuration decoded from the module Config proto.
struct AdapterRunConfig {
    std::vector<std::string> test_names;  // empty = module default (non-destructive set)
    int      interval_ms      = 1000;     // CommonConfig::interval_ms
    long long duration_ms     = 0;        // CommonConfig::duration_ms
    bool     run_forever      = false;    // CommonConfig::run_forever
    int      poll_interval_ms = 1000;     // CommonConfig::poll_interval_ms
    bool     config_present   = false;    // true when a Config message was decoded
};

// Decode serialized Any bytes carrying the module's Config message.
// Returns false when bytes are present but do not decode (caller must NOT run
// tests with unknown semantics; return a config error result instead).
// Empty input is accepted: the module runs with defaults.
bool decode_adapter_config(const std::string& module_id,
                           const std::vector<uint8_t>& config_bytes,
                           AdapterRunConfig& out);

// Push the decoded configuration into the HAL module (HAL-001 API) and return
// the test list to run: the explicit configured list when present, otherwise
// every available NON-destructive test.
std::vector<std::string> select_adapter_tests(std::vector<std::string> available_tests,
                                              const AdapterRunConfig& cfg);

// Forward the decoded common configuration to the HAL module via the
// TestBase::configure() API (HAL-001), so interval/duration/test selection
// reach the module implementation.
void apply_adapter_config(TestBase& hal, const AdapterRunConfig& cfg);

// True for tests that can reset/reboot the board, arm the watchdog, or write
// to storage. Such tests only run when the server explicitly lists them.
bool is_destructive_test(const std::string& test_name);

// Build a single ERROR TestResult for a module whose config could not be
// decoded (prevents silently falling back to defaults).
hal::TestResult make_config_error_result(const std::string& module_id,
                                         const std::string& message);

} // namespace acadia::client