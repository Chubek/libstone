#pragma once

#include "traceml.hpp"

namespace limestone::traceml {
// Generates a C99 translation unit exporting traceml_generated_<module>_run.
// module is an ASCII identifier fragment; closed forms are not pre-evaluated.
Result<std::string> emit_c99(const Program&,std::string_view module="main");
}
