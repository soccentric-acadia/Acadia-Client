// registry/adapters/thermal_adapter.cc — thermal_management HAL adapter.

#include "registry/adapters/thermal_adapter.h"

// --- On-target only --------------------------------------------------------
// #include "thermal_management_lib/thermal_management_lib.h"
// ---------------------------------------------------------------------------

#include <spdlog/spdlog.h>

namespace acadia::client {

ThermalAdapter::ThermalAdapter() {
    // hal_ = std::make_unique<ThermalManagementTesting>();
}

ThermalAdapter::~ThermalAdapter() = default;

hal::TestResult ThermalAdapter::make_result(const std::string& test_id,
                                              const hal::TestOutput& output) const {
    hal::TestResult r;
    r.module_id = "thermal_management";
    r.test_id   = test_id;
    r.name      = test_id;
    r.output    = output;
    return r;
}

std::vector<hal::TestResult> ThermalAdapter::run(int mode,
                                                   const std::vector<uint8_t>& config_bytes) {
    (void)mode;
    (void)config_bytes;

    // Stub — replace with hal_->runAllTests() / runSelectedTests() when HAL linked.
    spdlog::warn("thermal_adapter: running in stub mode — no HAL library linked");

    std::vector<hal::TestResult> results;

    // testSocJunctionTemperature
    {
        hal::TestOutput out;
        out.status   = hal::Status::PASS;
        out.severity = hal::Severity::INFO;
        out.metrics["soc_junction_temp_c"] = double{45.2};
        // MONITORING mode: also populate monitoring map
        if (mode == 3 /* MONITORING */) {
            out.monitoring["soc_junction_temp_c"] = double{45.2};
        }
        out.reason["note"] = "stub: thermal_management_lib not linked";
        results.push_back(make_result("testSocJunctionTemperature", out));
    }

    // testCpuTemperature
    {
        hal::TestOutput out;
        out.status   = hal::Status::PASS;
        out.severity = hal::Severity::INFO;
        out.metrics["cpu_temp_c"] = double{52.7};
        if (mode == 3) out.monitoring["cpu_temp_c"] = double{52.7};
        out.reason["note"] = "stub: thermal_management_lib not linked";
        results.push_back(make_result("testCpuTemperature", out));
    }

    return results;
}

} // namespace acadia::client
