#pragma once
// registry/adapters/thermal_management_adapter.h — adapter for the "thermal_management" HAL module.

#include "registry/AdapterBase.h"
#include <memory>
#include <vector>

class ThermalManagementTesting;  // forward declaration

namespace acadia::client {

class ThermalManagementAdapter final : public AdapterBase {
public:
    ThermalManagementAdapter();
    ~ThermalManagementAdapter() override;

    std::string module_id() const override { return "thermal_management"; }
    std::vector<hal::TestResult> run(int mode,
                                      const std::vector<uint8_t>& config_bytes) override;
private:
    std::unique_ptr<ThermalManagementTesting> hal_;
};

} // namespace acadia::client
