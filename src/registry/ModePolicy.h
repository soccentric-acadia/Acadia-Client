#pragma once
// registry/ModePolicy.h — CLI-118: check a requested ExecutionMode against
// the module's declared `modes` list from modules.yaml.
//
// The list in modules.yaml is treated as a RESTRICTION only when non-empty:
// a module with no declared modes is not restricted (backward compatible
// with first-generation modules.yaml files that omit `modes`).

#include "common.pb.h"

#include <cctype>
#include <string>
#include <vector>

namespace acadia::client {

namespace detail {

inline bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Canonical proto names plus the legacy aliases documented in
// protos/common.proto (ONE_SHOT, EVENT).
inline const std::vector<std::string>& mode_names(hal::common::ExecutionMode mode) {
    static const std::vector<std::string> kEmpty;
    switch (mode) {
        case hal::common::SINGLE_SHOT: {
            static const std::vector<std::string> v{"SINGLE_SHOT", "ONE_SHOT"};
            return v;
        }
        case hal::common::LONG_TERM: {
            static const std::vector<std::string> v{"LONG_TERM"};
            return v;
        }
        case hal::common::MONITORING: {
            static const std::vector<std::string> v{"MONITORING"};
            return v;
        }
        case hal::common::EVENT_BASED: {
            static const std::vector<std::string> v{"EVENT_BASED", "EVENT"};
            return v;
        }
        default:
            return kEmpty;
    }
}

} // namespace detail

// True when `mode` is a known execution mode and, if `declared` is non-empty,
// one of its names (canonical or alias, case-insensitive) is declared.
inline bool mode_supported(const std::vector<std::string>& declared,
                           hal::common::ExecutionMode mode) {
    const auto& names = detail::mode_names(mode);
    if (names.empty()) return false;   // UNSPECIFIED or unknown wire value
    if (declared.empty()) return true; // no restriction declared
    for (const auto& d : declared)
        for (const auto& name : names)
            if (detail::ieq(d, name)) return true;
    return false;
}

} // namespace acadia::client
