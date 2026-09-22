// registry/adapters/poe_hat_adapter.cc — poe_hat HAL module adapter.
// HARDWARE-UNVERIFIED: calls real HAL; correctness requires on-target execution.

#include "registry/adapters/poe_hat_adapter.h"
#include "poe_hat_lib/poe_hat_lib.h"

#include <algorithm>
#include "registry/AdapterConfig.h"

namespace acadia::client {

PoeHatAdapter::PoeHatAdapter() : hal_(std::make_unique<PoeHatTesting>()) {}
PoeHatAdapter::~PoeHatAdapter() = default;

std::vector<hal::TestResult> PoeHatAdapter::run(int mode,
                                                      const std::vector<uint8_t>& config_bytes) {
    AdapterRunConfig rc;
    if (!decode_adapter_config("poe_hat", config_bytes, rc)) {
        // CLI-160: a malformed config must surface as an ERROR result,
        // never fall back to HAL defaults.
        return {make_config_error_result("poe_hat", "unparseable module config")};
    }
    apply_adapter_config(*hal_, rc);  // HAL-001: push config into the module
    auto test_names = select_adapter_tests(hal_->getAvailableTests(), rc);
    auto results    = hal_->runSelectedTests(test_names);
    for (auto& r : results) {
        if (r.module_id.empty()) r.module_id = "poe_hat";
    }
    if (mode == 3 /* MONITORING */) {
        for (auto& r : results) {
            for (const auto& [k, v] : r.output.metrics)
                r.output.monitoring.emplace(k, v);
        }
    }
    if (mode == 4 /* EVENT_BASED */) {
        bool event = std::any_of(results.begin(), results.end(),
            [](const hal::TestResult& r) {
                return r.output.status != hal::Status::PASS;
            });
        if (!event) return {};
    }
    return results;
}

} // namespace acadia::client
