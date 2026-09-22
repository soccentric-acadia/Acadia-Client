// registry/adapters/usb_adapter.cc — usb HAL module adapter.
// HARDWARE-UNVERIFIED: calls real HAL; correctness requires on-target execution.

#include "registry/adapters/usb_adapter.h"
#include "usb_lib/usb_lib.h"

#include <algorithm>
#include "registry/AdapterConfig.h"

namespace acadia::client {

UsbAdapter::UsbAdapter() : hal_(std::make_unique<UsbTesting>()) {}
UsbAdapter::~UsbAdapter() = default;

std::vector<hal::TestResult> UsbAdapter::run(int mode,
                                                      const std::vector<uint8_t>& config_bytes) {
    AdapterRunConfig rc;
    if (!decode_adapter_config("usb", config_bytes, rc)) {
        // CLI-160: a malformed config must surface as an ERROR result,
        // never fall back to HAL defaults.
        return {make_config_error_result("usb", "unparseable module config")};
    }
    apply_adapter_config(*hal_, rc);  // HAL-001: push config into the module
    auto test_names = select_adapter_tests(hal_->getAvailableTests(), rc);
    auto results    = hal_->runSelectedTests(test_names);
    for (auto& r : results) {
        if (r.module_id.empty()) r.module_id = "usb";
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
