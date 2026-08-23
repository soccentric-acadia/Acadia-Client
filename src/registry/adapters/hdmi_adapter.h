#pragma once
// registry/adapters/hdmi_adapter.h — adapter for the "hdmi" HAL module (HDMI output).

#include "registry/AdapterBase.h"
#include <memory>
#include <vector>

class HdmiTesting;  // forward declaration

namespace acadia::client {

class HdmiAdapter final : public AdapterBase {
public:
    HdmiAdapter();
    ~HdmiAdapter() override;

    std::string module_id() const override { return "hdmi"; }
    std::vector<hal::TestResult> run(int mode,
                                      const std::vector<uint8_t>& config_bytes) override;
private:
    std::unique_ptr<HdmiTesting> hal_;
};

} // namespace acadia::client
