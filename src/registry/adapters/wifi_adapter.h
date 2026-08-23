#pragma once
// registry/adapters/wifi_adapter.h — adapter for the "wifi" HAL module (Wi-Fi radio).

#include "registry/AdapterBase.h"
#include <memory>
#include <vector>

class WifiTesting;  // forward declaration

namespace acadia::client {

class WifiAdapter final : public AdapterBase {
public:
    WifiAdapter();
    ~WifiAdapter() override;

    std::string module_id() const override { return "wifi"; }
    std::vector<hal::TestResult> run(int mode,
                                      const std::vector<uint8_t>& config_bytes) override;
private:
    std::unique_ptr<WifiTesting> hal_;
};

} // namespace acadia::client
