#ifndef XBIND_DISCOVERY_HPP
#define XBIND_DISCOVERY_HPP

#include "xbind_qamrpp.hpp"

#include <filesystem>
#include <string_view>

namespace xbind::discovery {
bool available() noexcept;

// Offline only. ARCH uses qBFD architecture spelling and is required for
// universal Mach-O. The inventory never supplies signatures.
std::string inventory(const std::filesystem::path& image, std::string_view arch = {});

// Match manifest C declarations to public exports; not a signature check.
std::string bind(const std::filesystem::path& image, const qamrpp::Manifest& manifest,
                 std::string_view arch = {});
}

#endif
