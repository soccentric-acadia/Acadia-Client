// registry/adapters/system_memory_adapter.cc — system_memory HAL adapter.

#include "registry/adapters/system_memory_adapter.h"

// --- On-target only --------------------------------------------------------
// #include "system_memory_lib/system_memory_lib.h"
// ---------------------------------------------------------------------------

#include <spdlog/spdlog.h>

namespace acadia::client {

SystemMemoryAdapter::SystemMemoryAdapter() {
    // hal_ = std::make_unique<SystemMemoryTesting>();
}

SystemMemoryAdapter::~SystemMemoryAdapter() = default;

hal::TestResult SystemMemoryAdapter::make_result(const std::string& test_id,
                                                   const hal::TestOutput& output) const {
    hal::TestResult r;
    r.module_id = "system_memory";
    r.test_id   = test_id;
    r.name      = test_id;
    r.output    = output;
    return r;
}

std::vector<hal::TestResult> SystemMemoryAdapter::run(int mode,
                                                       const std::vector<uint8_t>& config_bytes) {
    (void)mode;
    (void)config_bytes;

    spdlog::warn("system_memory_adapter: running in stub mode — no HAL library linked");

    std::vector<hal::TestResult> results;

    // testMemoryBandwidth
    {
        hal::TestOutput out;
        out.status   = hal::Status::PASS;
        out.severity = hal::Severity::INFO;
        out.metrics["read_bandwidth_mbps"]  = double{38000.0};
        out.metrics["write_bandwidth_mbps"] = double{34000.0};
        if (mode == 3) {
            out.monitoring["read_bandwidth_mbps"]  = double{38000.0};
            out.monitoring["write_bandwidth_mbps"] = double{34000.0};
        }
        out.reason["note"] = "stub: system_memory_lib not linked";
        results.push_back(make_result("testMemoryBandwidth", out));
    }

    // testMemoryLatencyMeasurements
    {
        hal::TestOutput out;
        out.status   = hal::Status::PASS;
        out.severity = hal::Severity::INFO;
        out.metrics["random_access_latency_ns"] = double{85.0};
        out.reason["note"] = "stub: system_memory_lib not linked";
        results.push_back(make_result("testMemoryLatencyMeasurements", out));
    }

    return results;
}

} // namespace acadia::client
