// registry/adapters/cpu_adapter.cc — CPU HAL module adapter.
//
// Production build: uncomment the CpuTesting include and member.
// Development/CI build: stub implementation used until the HAL library ships.

#include "registry/adapters/cpu_adapter.h"

// --- On-target only --------------------------------------------------------
// #include "cpu_lib/cpu_lib.h"
// ---------------------------------------------------------------------------

#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace acadia::client {

CpuAdapter::CpuAdapter() {
    // On-target: hal_ = std::make_unique<CpuTesting>();
}

CpuAdapter::~CpuAdapter() = default;

hal::TestResult CpuAdapter::make_result(const std::string& test_id,
                                          const hal::TestOutput& output) const {
    hal::TestResult r;
    r.module_id = "cpu";
    r.test_id   = test_id;
    r.name      = test_id;
    // sequence and timestamp are stamped by RunOrchestrator
    r.output    = output;
    return r;
}

std::vector<hal::TestResult> CpuAdapter::run(int mode,
                                               const std::vector<uint8_t>& config_bytes) {
    // -----------------------------------------------------------------------
    // Stub implementation — returns a representative set of results.
    // Replace with real CpuTesting calls when the HAL library is linked.
    // -----------------------------------------------------------------------
    (void)mode;
    (void)config_bytes;

    // On-target implementation sketch (commented to avoid compile errors
    // without the HAL library):
    //
    // std::vector<std::string> test_names;
    // // Parse config_bytes into hal.modules.cpu.Config protobuf, extract test_names.
    // ...
    // std::vector<hal::TestResult> results;
    // if (test_names.empty()) {
    //     // Run all tests.
    //     auto outputs = hal_->runAllTests();
    //     for (const auto& tr : outputs) results.push_back(tr); // already TestResult
    // } else {
    //     auto selected = hal_->runSelectedTests(test_names);
    //     for (const auto& tr : selected) results.push_back(tr);
    // }
    // return results;

    spdlog::warn("cpu_adapter: running in stub mode — no HAL library linked");

    std::vector<hal::TestResult> results;

    // testAll12CpuCoresFunctionality — stub
    {
        hal::TestOutput out;
        out.status   = hal::Status::PASS;
        out.severity = hal::Severity::INFO;
        // Stub: indicate all 4 cores functional on Raspberry Pi 5
        out.metrics["core_count"] = int64_t{4};
        out.reason["note"] = "stub: cpu_lib not linked";
        results.push_back(make_result("testAll12CpuCoresFunctionality", out));
    }

    // testCpuCoreFrequencyScaling — stub
    {
        hal::TestOutput out;
        out.status   = hal::Status::PASS;
        out.severity = hal::Severity::INFO;
        out.metrics["min_freq_khz"] = int64_t{600000};
        out.metrics["max_freq_khz"] = int64_t{2400000};
        out.reason["note"] = "stub: cpu_lib not linked";
        results.push_back(make_result("testCpuCoreFrequencyScaling", out));
    }

    return results;
}

} // namespace acadia::client
