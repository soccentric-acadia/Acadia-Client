#pragma once
// registry/AdapterRegistry.h — owns all AdapterBase instances and dispatches
// HAL invocations by module_id.
//
// Architectural rules:
//  - The only code that may call adapter->run() is AdapterRegistry::invoke().
//  - No switch(module_id) anywhere outside AdapterRegistry.
//  - Adapters self-register via register_adapter(); typically called from a
//    static-initialiser or App::init().

#include "registry/AdapterBase.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace acadia::client {

class AdapterRegistry {
public:
    AdapterRegistry() = default;
    ~AdapterRegistry() = default;

    AdapterRegistry(const AdapterRegistry&)            = delete;
    AdapterRegistry& operator=(const AdapterRegistry&) = delete;

    // Register an adapter.  Replaces any existing adapter for the same module_id.
    void register_adapter(std::unique_ptr<AdapterBase> adapter);

    // Returns true when an adapter for `module_id` is registered.
    bool has_adapter(const std::string& module_id) const;

    // Invoke the adapter for `module_id`.
    // Returns an empty vector (with a logged warning) if no adapter is registered.
    std::vector<hal::TestResult> invoke(const std::string& module_id,
                                         int mode,
                                         const std::vector<uint8_t>& config_bytes);

    // List all registered module IDs (sorted).
    std::vector<std::string> registered_ids() const;

    // CLI-104/116: true when the module is backed by the honest stub
    // (no real HAL implementation behind it).
    bool is_stub_adapter(const std::string& module_id) const;

private:
    std::unordered_map<std::string, std::unique_ptr<AdapterBase>> adapters_;
    // CLI-112: adapters/HAL classes are not thread-safe; every invoke is
    // serialized so the HalService and the RunOrchestrator cannot drive the
    // same adapter concurrently.
    mutable std::mutex invoke_mtx_;
};

} // namespace acadia::client
