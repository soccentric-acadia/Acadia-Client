#pragma once
// registry/adapters/stub_adapter.h — Generic stub adapter used for any HAL
// module that does not yet have a purpose-built adapter.
//
// Returns a single TestResult with status = ERROR and an error_message
// indicating that no real adapter has been wired up.  This prevents the runner
// from crashing when a module is present in the registry but not yet
// implemented.

#include "registry/AdapterBase.h"

#include <string>
#include <vector>

namespace acadia::client {

class StubAdapter final : public AdapterBase {
public:
    explicit StubAdapter(std::string id) : id_(std::move(id)) {}

    std::string module_id() const override { return id_; }

    std::vector<hal::TestResult> run(int /*mode*/,
                                      const std::vector<uint8_t>& /*config*/) override;

private:
    std::string id_;
};

} // namespace acadia::client
