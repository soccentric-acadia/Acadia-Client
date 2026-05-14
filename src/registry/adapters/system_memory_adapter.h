#pragma once
// registry/adapters/system_memory_adapter.h — adapter for the
// "system_memory" HAL module.
//
// Bandwidth and latency measurements → `metrics` as doubles/int64.
// MONITORING mode also populates `monitoring`.

#include "registry/AdapterBase.h"

#include <string>
#include <vector>

namespace acadia::client {

class SystemMemoryAdapter final : public AdapterBase {
public:
    SystemMemoryAdapter();
    ~SystemMemoryAdapter() override;

    std::string module_id() const override { return "system_memory"; }

    std::vector<hal::TestResult> run(int mode,
                                      const std::vector<uint8_t>& config_bytes) override;

private:
    hal::TestResult make_result(const std::string& test_id,
                                 const hal::TestOutput& output) const;
    // std::unique_ptr<SystemMemoryTesting> hal_;
};

} // namespace acadia::client
