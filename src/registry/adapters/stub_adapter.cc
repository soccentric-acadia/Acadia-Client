#include "registry/adapters/stub_adapter.h"

namespace acadia::client {

std::vector<hal::TestResult> StubAdapter::run(int /*mode*/,
                                               const std::vector<uint8_t>& /*config*/) {
    hal::TestResult r;
    r.module_id         = id_;
    r.test_id           = id_;
    r.name              = id_;
    // sequence and timestamp are stamped by the runner
    r.output.status     = hal::Status::ERROR;
    r.output.severity   = hal::Severity::ERROR;
    r.output.error_message =
        "stub adapter: no real implementation for module_id=" + id_ +
        "; link the HAL library and register a real adapter";
    return {std::move(r)};
}

} // namespace acadia::client
