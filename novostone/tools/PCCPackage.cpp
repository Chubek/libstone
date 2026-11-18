#include "novostone/tools/PCCPackage.hpp"
#include "novostone/PCC/Package.hpp"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace novostone::tools {
namespace {

std::ifstream open_input(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot open PCC input: " + std::string(path));
  return file;
}

std::string read_input(const char* path, std::size_t limit) {
  auto file = open_input(path);
  std::string result;
  char buffer[1024];
  while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
    const auto count = static_cast<std::size_t>(file.gcount());
    if (count > limit - result.size())
      throw std::invalid_argument("PCC input exceeds byte limit");
    result.append(buffer, count);
  }
  if (file.bad() || !file.eof()) throw std::runtime_error("cannot read PCC input");
  return result;
}

void output(std::string_view message) {
  std::cout.write(message.data(), static_cast<std::streamsize>(message.size()));
  std::cout.flush();
  if (!std::cout) throw std::runtime_error("cannot write PCC output");
}

void select_transforms(pcc::RewriteConstraints& constraints, std::string_view list) {
  constraints.allowed.clear();
  for (;;) {
    const auto end = list.find(',');
    constraints.allow(list.substr(0, end));
    if (end == std::string_view::npos) return;
    list.remove_prefix(end + 1);
  }
}

int pack(int argc, char** argv) {
  if (argc < 5 || std::string_view(argv[3]) != "--sign-key")
    throw std::invalid_argument("usage: pcc-pack SOURCE --sign-key PEM "
                                "[--transforms LIST] [--max-rewrites N]");
  const pcc::PackagePolicy policy;
  pcc::RewriteConstraints constraints;
  constraints.limits = policy.max_work;
  bool transforms_seen = false, rewrites_seen = false;
  for (int i = 5; i < argc; i += 2) {
    const std::string_view option(argv[i]);
    if (i + 1 == argc) throw std::invalid_argument("missing PCC package option value");
    const std::string_view value(argv[i + 1]);
    if (option == "--transforms" && !transforms_seen) {
      select_transforms(constraints, value);
      transforms_seen = true;
    } else if (option == "--max-rewrites" && !rewrites_seen) {
      auto& count = constraints.limits.max_rewrites;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          count > policy.max_work.max_rewrites)
        throw std::invalid_argument("invalid PCC package rewrite limit");
      rewrites_seen = true;
    } else {
      throw std::invalid_argument("unknown or duplicate PCC package option");
    }
  }
  const auto key = read_input(argv[4], pcc::package_max_key_bytes);
  const auto source = ir::parse(read_input(argv[2], policy.max_package_bytes));
  const auto artifact = pcc::build(source, constraints);
#ifdef _WIN32
  if (_setmode(_fileno(stdout), _O_BINARY) == -1)
    throw std::runtime_error("cannot set binary PCC output");
#endif
  output(pcc::sign_package(artifact, key, policy));
  return 0;
}

} // namespace

std::optional<int> pcc_package_command(int argc, char** argv) {
  if (argc < 2) return std::nullopt;
  const std::string_view command(argv[1]);
  if (command == "pcc-pack") return pack(argc, argv);
  if (command == "pcc-verify") {
    if (argc != 5 || std::string_view(argv[3]) != "--trust-key")
      throw std::invalid_argument("usage: pcc-verify PACKAGE --trust-key PEM");
    auto file = open_input(argv[2]);
    const auto package = pcc::read_package(file);
    const auto key = read_input(argv[4], pcc::package_max_key_bytes);
    (void)pcc::verify_package(package, key);
    output("PCC-VERIFY/1 OK\n");
    return 0;
  }
  if (command == "pcc-serve") {
    if (argc != 4 || std::string_view(argv[2]) != "--trust-key")
      throw std::invalid_argument("usage: pcc-serve --trust-key PEM");
    // The configured command takes no path, script, options, solver arguments,
    // or original SSH command from the request body/environment.
    try {
#ifdef _WIN32
      if (_setmode(_fileno(stdin), _O_BINARY) == -1)
        throw std::runtime_error("cannot set binary PCC input");
#endif
      const auto key = read_input(argv[3], pcc::package_max_key_bytes);
      const auto package = pcc::read_package(std::cin);
      (void)pcc::verify_package(package, key);
      output("PCC-VERIFY/1 OK\n");
      return 0;
    } catch (const std::exception&) {
      // A transport failure is still failure (including if output() throws).
      // Do not expose untrusted diagnostics or verifier internals to peers.
      output("PCC-VERIFY/1 REJECT\n");
      return 1;
    }
  }
  return std::nullopt;
}

} // namespace novostone::tools
