#include "xbind.h"
#include "xbind_qamrpp.hpp"
#include "xbind_codegen.hpp"
#include "xbind_discovery.hpp"

#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef XBIND_SOURCE_DIR
#define XBIND_SOURCE_DIR "."
#endif

namespace {

bool has_suffix(const std::string &value, const std::string &suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool file_exists(const std::string &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return false;
  std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
  return input.good();
}

std::string data_directory(const char* argv0) {
  namespace fs = std::filesystem;
  if (const char* data = std::getenv("XBIND_DATA_DIR"); data && *data) return data;
  auto has_map = [](const fs::path& dir) {
    return file_exists((dir / "typedefs/Lua-Default.sexpr").string());
  };
  fs::path executable(argv0);
  if (!executable.has_parent_path()) {
    if (const char* path = std::getenv("PATH")) {
      std::string_view dirs(path);
#ifdef _WIN32
      constexpr char separator = ';';
#else
      constexpr char separator = ':';
#endif
      for (;;) {
        auto end = dirs.find(separator);
        auto candidate = fs::path(dirs.substr(0, end)) / executable;
#ifdef _WIN32
        if (!candidate.has_extension()) candidate += ".exe";
#endif
        if (file_exists(candidate.string())) {
          executable = std::move(candidate);
          break;
        }
        if (end == std::string_view::npos) break;
        dirs.remove_prefix(end + 1);
      }
    }
  }
  std::error_code error;
  auto canonical = fs::canonical(executable, error);
  if (!error) {
    const auto relocated = canonical.parent_path() / XBIND_RELATIVE_DATA_DIR;
    if (has_map(relocated)) return relocated.string();
  }
  if (has_map(XBIND_INSTALL_DATA_DIR)) return XBIND_INSTALL_DATA_DIR;
  return XBIND_SOURCE_DIR;
}

void print_usage(std::ostream &stream) {
  stream << "usage: xbind <command> [args]\n"
         << "\n"
         << "commands:\n"
         << "  --help                         Show this help.\n"
         << "  --version                      Print the XBind version.\n"
         << "  --list-targets                 List registered target names.\n"
         << "  --capabilities                 Distinguish emitters from scaffolds (JSON).\n"
         << "  --target NAME                  Show one target's scaffold paths.\n"
         << "  --show-paths [ROOT]            Show key XBind source directories.\n"
         << "  --check-layout [ROOT]          Validate the scaffold layout.\n"
         << "  --find-spec LIB [DIR]          Resolve LIB.xbind or LIB.lua.\n"
         << "  --check-spec LIB [DIR]         Execute and validate a Lua spec with QaMRpp.\n"
         << "  --dump-spec LIB [DIR]          Execute and summarize a Lua spec with QaMRpp.\n"
         << "  --dump-ir LIB [DIR]            Write deterministic spec IR as JSON.\n"
         << "  --discover IMAGE [ARCH]        Inventory an image without loading it.\n"
         << "  --bind-ir IMAGE SPEC [ARCH]    Match explicit declarations to image exports.\n"
         << "  --emit-check LIB [DIR]         Write C++20 exact-signature assertions.\n"
         << "  --emit-lua LIB [DIR]           Write Lua 5.4 glue through DomLuaBind.\n";
}

void print_target(const xbind_target_info &target) {
  std::cout << target.name << "\n";
  std::cout << "  compiler: " << target.compiler_directory << "\n";
  std::cout << "  dombind:  " << target.dombind_directory << "\n";
  std::cout << "  typedefs: " << target.typedef_file << "\n";
  std::cout << "  native:   " << target.native_boundary << "\n";
}

void layout_diagnostic(void *user, const char *message) {
  auto *stream = static_cast<std::ostream *>(user);
  (*stream) << message << "\n";
}

std::string resolve_spec_or_throw(const std::string &library,
                                  const std::string &directory) {
  if ((has_suffix(library, ".xbind") || has_suffix(library, ".lua")) &&
      file_exists(library)) {
    return library;
  }

  char resolved[4096];
  const xbind_status status =
      xbind_resolve_spec(directory.c_str(), library.c_str(), resolved, sizeof(resolved));
  if (status != XBIND_STATUS_OK) {
    throw std::runtime_error(library + ": " + xbind_status_string(status));
  }
  return resolved;
}

int command_spec(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    std::cerr << "xbind: " << argv[1]
              << " requires a library name or spec path\n";
    return 2;
  }

  const std::string directory = argc >= 4 ? argv[3] : ".";
  try {
    const std::string spec = resolve_spec_or_throw(argv[2], directory);
    const xbind::qamrpp::Manifest manifest = xbind::qamrpp::load_spec_file(spec);
    if (std::strcmp(argv[1], "--dump-ir") == 0) {
      std::cout << xbind::codegen::dump_ir(manifest);
    } else if (std::strcmp(argv[1], "--emit-check") == 0) {
      std::cout << xbind::codegen::emit_check(manifest);
    } else if (std::strcmp(argv[1], "--emit-lua") == 0) {
      std::cout << xbind::codegen::emit_lua(manifest, data_directory(argv[0]));
    } else if (std::strcmp(argv[1], "--dump-spec") == 0) {
      std::cout << xbind::qamrpp::dump_manifest(manifest);
    } else {
      std::cout << "XBind spec OK: " << manifest.name << " (" << spec << ")\n";
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "xbind: " << error.what() << "\n";
    return 1;
  }
}

int command_image(int argc, char** argv, bool bind) {
  const int required = bind ? 4 : 3;
  if (argc != required && argc != required + 1) {
    std::cerr << "xbind: " << argv[1] << (bind ? " requires IMAGE SPEC [ARCH]\n" : " requires IMAGE [ARCH]\n");
    return 2;
  }
  try {
    if (!xbind::discovery::available())
      throw std::runtime_error("binary discovery was not built (requires qBFD)");
    const char* arch = argc > required ? argv[required] : "";
    if (bind) {
      auto manifest = xbind::qamrpp::load_spec_file(argv[3]);
      std::cout << xbind::discovery::bind(argv[2], manifest, arch);
    } else {
      std::cout << xbind::discovery::inventory(argv[2], arch);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "xbind: " << error.what() << '\n';
    return 1;
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc <= 1 || std::strcmp(argv[1], "--help") == 0) {
    print_usage(std::cout);
    return 0;
  }

  if (std::strcmp(argv[1], "--version") == 0) {
    std::cout << xbind_version() << "\n";
    return 0;
  }

  if (std::strcmp(argv[1], "--list-targets") == 0) {
    for (size_t index = 0; index < xbind_target_count(); ++index) {
      std::cout << xbind_target_at(index)->name << "\n";
    }
    return 0;
  }
  if (std::strcmp(argv[1], "--capabilities") == 0) {
    std::cout << "{\"binary_discovery\":" << (xbind::discovery::available() ? "true" : "false")
              << ",\"targets\":[";
    for (size_t i = 0; i < xbind_target_count(); ++i) {
      if (i) std::cout << ',';
      const auto* target = xbind_target_at(i);
      const bool lua = std::strcmp(target->name, "Lua") == 0;
      std::cout << "{\"name\":\"" << target->name << "\",\"backend\":\""
                << (lua ? "lua54-direct-link" : "scaffold-only") << "\"}";
    }
    std::cout << "]}\n";
    return 0;
  }
  if (std::strcmp(argv[1], "--discover") == 0) return command_image(argc, argv, false);
  if (std::strcmp(argv[1], "--bind-ir") == 0) return command_image(argc, argv, true);

  if (std::strcmp(argv[1], "--target") == 0) {
    if (argc < 3) {
      std::cerr << "xbind: --target requires a target name\n";
      return 2;
    }
    const xbind_target_info *target = xbind_find_target(argv[2]);
    if (!target) {
      std::cerr << "xbind: unknown target: " << argv[2] << "\n";
      return 1;
    }
    print_target(*target);
    return 0;
  }

  if (std::strcmp(argv[1], "--show-paths") == 0) {
    const char *root = argc >= 3 ? argv[2] : XBIND_SOURCE_DIR;
    std::cout << "root:     " << root << "\n";
    std::cout << "lxbind:   " << root << "/lxbind\n";
    std::cout << "stdlib:   " << root << "/stdlib\n";
    std::cout << "typedefs: " << root << "/typedefs\n";
    std::cout << "dombind:  " << root << "/dombind\n";
    return 0;
  }

  if (std::strcmp(argv[1], "--check-layout") == 0) {
    const char *root = argc >= 3 ? argv[2] : XBIND_SOURCE_DIR;
    size_t errors = xbind_check_layout(root, layout_diagnostic, &std::cerr);
    if (errors == 0) {
      std::cout << "XBind scaffold layout OK: " << root << "\n";
      return 0;
    }
    std::cerr << "xbind: " << errors << " layout issue(s)\n";
    return 1;
  }

  if (std::strcmp(argv[1], "--find-spec") == 0) {
    if (argc < 3) {
      std::cerr << "xbind: --find-spec requires a library name\n";
      return 2;
    }
    try {
      std::cout << resolve_spec_or_throw(argv[2], argc >= 4 ? argv[3] : ".") << "\n";
      return 0;
    } catch (const std::exception &error) {
      std::cerr << "xbind: " << error.what() << "\n";
      return 1;
    }
  }

  if (std::strcmp(argv[1], "--check-spec") == 0 || std::strcmp(argv[1], "--dump-spec") == 0 ||
      std::strcmp(argv[1], "--dump-ir") == 0 || std::strcmp(argv[1], "--emit-check") == 0 ||
      std::strcmp(argv[1], "--emit-lua") == 0) {
    return command_spec(argc, argv);
  }

  std::cerr << "xbind: unknown command: " << argv[1] << "\n";
  print_usage(std::cerr);
  return 2;
}
