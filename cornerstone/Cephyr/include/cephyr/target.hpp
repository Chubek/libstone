#ifndef CEPHYR_TARGET_HPP
#define CEPHYR_TARGET_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifndef CEPHYR_DEFAULT_TARGET_TRIPLE
#define CEPHYR_DEFAULT_TARGET_TRIPLE "x86_64-linux-gnu"
#endif

#ifndef CEPHYR_BUILTIN_TARGET_BANK
#define CEPHYR_BUILTIN_TARGET_BANK ""
#endif

namespace cephyr {

class Diagnostics;

inline constexpr const char *default_target_triple = CEPHYR_DEFAULT_TARGET_TRIPLE;

struct Target {
    std::string triple;
    std::string architecture;
    std::string abi;
    bool is_linux = false;
    std::filesystem::path specification;
    std::vector<std::string> assembler;
    std::vector<std::string> linker;
    std::vector<std::string> archiver;
    std::vector<std::string> assemble_flags;
    std::vector<std::string> link_flags;
};

// Loads script when it is non-empty, otherwise <bank>/<triple>.lua. If bank is
// also empty, CEPHYR_BUILTIN_TARGET_BANK supplies the bank. The Lua chunk must
// return one target table; it runs without Lua's native file/process libraries.
// An empty triple is allowed only with an explicit script and uses its triple.
std::optional<Target> load_target(const std::string &triple,
                                  const std::filesystem::path &bank,
                                  const std::filesystem::path &script,
                                  const std::filesystem::path &archon_root,
                                  Diagnostics &diagnostics);

} // namespace cephyr

#endif
