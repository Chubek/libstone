#pragma once
#include "sandstone.hpp"

namespace domsymas::cc {
// Current DSL/schema version this compiler understands.
inline constexpr uint32_t kSchemaVersion = 1;

// Result of a compile step.
struct Result {
    bool ok = false;
    std::string error;                 // human-readable diagnostic
    std::string spec_json;             // the JSON document (for --emit-spec)
};

Result compile_assembler_records(const std::string& json, domsymas::Builder& builder);
} // namespace domsymas::cc
