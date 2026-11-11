#include "Backends.hpp"
#include <sstream>

namespace sandstone::archml {
std::vector<std::string_view> target_names() { return {"cpp", "json", "archml"}; }
std::unique_ptr<Target> make_target(std::string_view name) {
    if (name == "cpp") return cpp_target();
    if (name == "json") return data_target(true);
    if (name == "archml") return data_target(false);
    throw std::invalid_argument("unknown generation target '" + std::string(name) + "'");
}
std::string generate(const Document &doc, std::string_view target, const TargetOptions &options) {
    std::ostringstream output;
    make_target(target)->generate(doc, output, options);
    return output.str();
}
}
