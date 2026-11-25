#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cephyr::detail {

struct Function;

std::string lower_mir_function(const Function &, std::string_view name,
                               std::size_t &module_instructions, bool optimize,
                               bool &mir_rewritten, std::uint64_t pass_mask);

} // namespace cephyr::detail
