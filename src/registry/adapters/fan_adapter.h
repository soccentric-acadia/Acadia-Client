#pragma once
// registry/adapters/fan_adapter.h — adapter for the "fan" HAL module (PWM cooling fan).

#include "registry/AdapterBase.h"
#include <memory>
#include <vector>

class FanTesting;  // forward declaration

namespace acadia::client {

class FanAdapter final : public AdapterBase {
public:
    FanAdapter();
    ~FanAdapter() override;

    std::string module_id() const override { return "fan"; }
    std::vector<hal::TestResult> run(int mode,
                                      const std::vector<uint8_t>& config_bytes) override;
private:
    std::unique_ptr<FanTesting> hal_;
};

} // namespace acadia::client
