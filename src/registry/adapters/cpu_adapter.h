#pragma once
// registry/adapters/cpu_adapter.h — adapter for the "cpu" HAL module.
//
// Wraps CpuTesting (from cpu_lib/cpu_lib.h) and maps its TestOutput
// results to the canonical hal::TestResult shape.
//
// Numeric values (frequencies, cache sizes, bandwidth) are stored in
// `metrics` as typed Values. Explanatory strings go to `reason`.
//
// Adapter rules (per Section 4.3 of req.md):
//  - Does NOT set sequence or timestamp (runner stamps those).
//  - Sets module_id = "cpu" (matches modules.yaml canonical id).
//  - Sets test_id = test function name (stable across renames).
//  - Sets error_message on exception or HAL precondition failure.
//  - Per-core results use subtests if the HAL provides them.

#include "registry/AdapterBase.h"

#include <string>
#include <vector>

namespace acadia::client {

class CpuAdapter final : public AdapterBase {
public:
    CpuAdapter();
    ~CpuAdapter() override;

    std::string module_id() const override { return "cpu"; }

    std::vector<hal::TestResult> run(int mode,
                                      const std::vector<uint8_t>& config_bytes) override;

private:
    // Converts a single TestOutput from CpuTesting into a TestResult.
    // test_id and name are the method name (e.g. "testCpuCoreFrequencyScaling").
    hal::TestResult make_result(const std::string& test_id,
                                 const hal::TestOutput& output) const;

    // NOTE: When the HAL library is linked on-target, instantiate CpuTesting here
    // as a member.  In the development/stub build it is absent.
    // std::unique_ptr<CpuTesting> hal_;
};

} // namespace acadia::client
