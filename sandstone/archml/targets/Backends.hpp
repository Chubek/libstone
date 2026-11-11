#pragma once
#include "Target.hpp"
namespace sandstone::archml {
std::unique_ptr<Target> cpp_target();
std::unique_ptr<Target> data_target(bool json);
}
