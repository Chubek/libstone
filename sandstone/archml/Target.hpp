#pragma once
#include "ArchML.hpp"
#include <memory>
#include <ostream>

namespace sandstone::archml {
struct TargetOptions {
    // Empty uses archml_<profile>. Explicit names must be nonreserved C++ identifiers.
    std::string namespace_name;
};
class Target {
public:
    virtual ~Target() = default;
    virtual std::string_view name() const = 0;
    virtual void generate(const Document &, std::ostream &, const TargetOptions & = {}) const = 0;
};
std::vector<std::string_view> target_names();
std::unique_ptr<Target> make_target(std::string_view name);
std::string generate(const Document &, std::string_view target, const TargetOptions & = {});
} // namespace sandstone::archml
