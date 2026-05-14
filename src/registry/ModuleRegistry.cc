#include "registry/ModuleRegistry.h"

#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>

namespace acadia::client {

int ModuleRegistry::load(const std::string& path) {
    YAML::Node root = YAML::LoadFile(path);
    const auto& entries = root["modules"];
    if (!entries || !entries.IsSequence()) {
        throw std::runtime_error("modules.yaml: no top-level 'modules' sequence at " + path);
    }

    std::unique_lock lock(mtx_);
    modules_.clear();

    int count = 0;
    for (const auto& node : entries) {
        ModuleEntry e;
        e.id              = node["id"]              ? node["id"].as<std::string>()              : "";
        e.name            = node["name"]            ? node["name"].as<std::string>()            : e.id;
        e.version         = node["version"]         ? node["version"].as<std::string>()         : "0.0.0";
        e.category        = node["category"]        ? node["category"].as<std::string>()        : "other";
        e.config_type_url = node["config_type_url"] ? node["config_type_url"].as<std::string>() : "";
        e.result_type_url = node["result_type_url"] ? node["result_type_url"].as<std::string>() : "";
        if (node["modes"] && node["modes"].IsSequence()) {
            for (const auto& m : node["modes"])
                e.modes.push_back(m.as<std::string>());
        }
        if (e.id.empty()) {
            spdlog::warn("registry: skipping module with empty id");
            continue;
        }
        modules_[e.id] = std::move(e);
        ++count;
    }
    // By default enable everything we know about.
    for (const auto& [id, _] : modules_) enabled_.insert(id);
    spdlog::info("registry: loaded {} modules from {}", count, path);
    return count;
}

std::vector<ModuleEntry> ModuleRegistry::all() const {
    std::shared_lock lock(mtx_);
    std::vector<ModuleEntry> v;
    v.reserve(modules_.size());
    for (const auto& [_, e] : modules_) v.push_back(e);
    return v;
}

bool ModuleRegistry::known(const std::string& id) const {
    std::shared_lock lock(mtx_);
    return modules_.count(id) > 0;
}

void ModuleRegistry::enable_all() {
    std::unique_lock lock(mtx_);
    for (const auto& [id, _] : modules_) enabled_.insert(id);
}

void ModuleRegistry::set_enabled(const std::unordered_set<std::string>& ids) {
    std::unique_lock lock(mtx_);
    enabled_ = ids;
}

bool ModuleRegistry::enabled(const std::string& id) const {
    std::shared_lock lock(mtx_);
    return enabled_.count(id) > 0;
}

} // namespace acadia::client
