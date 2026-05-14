#pragma once
// registry/adapters/thermal_adapter.h — adapter for the "thermal_management"
// HAL module.
//
// Wraps ThermalManagementTesting.  Temperature readings go to `metrics` as
// doubles (degrees C). MONITORING mode places the same readings in `monitoring`.

#include "registry/AdapterBase.h"

#include <string>
#include <vector>

namespace acadia::client {

class ThermalAdapter final : public AdapterBase {
public:
    ThermalAdapter();
    ~ThermalAdapter() override;

    std::string module_id() const override { return "thermal_management"; }

    std::vector<hal::TestResult> run(int mode,
                                      const std::vector<uint8_t>& config_bytes) override;

private:
    hal::TestResult make_result(const std::string& test_id,
                                 const hal::TestOutput& output) const;
    // std::unique_ptr<ThermalManagementTesting> hal_;
};

} // namespace acadia::client
