#pragma once

#include <asdl/schema.hpp>

namespace asdl {

struct GeneratedOutputs {
    std::string header;
    std::string source;
    std::string visitor;
    std::string listener;
    std::string builtins_header;
    std::string schema;
    std::string cpp_header;
};

// Generate in memory before publishing any files. Schema must be validated and
// contain the effective options. Always checks generated C symbol collisions.
GeneratedOutputs generate(const Schema& schema);

} // namespace asdl
