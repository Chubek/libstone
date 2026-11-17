#include "ld.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message ? message : "failed assertion");
}
static void image(const uint8_t *bytes, size_t size, bool pe) {
  require(bytes && size > 64, "missing image");
  require(pe ? bytes[0] == 'M' && bytes[1] == 'Z'
             : bytes[0] == 0xcf && bytes[1] == 0xfa && bytes[2] == 0xed && bytes[3] == 0xfe,
          "wrong image format");
}
int main(int argc, char **argv) try {
  require(argc == 4, "expected format, object, linker");
  bool pe = std::string(argv[1]) == "pe";
  auto link = pe ? qld_link_pe_bytes : qld_link_macho_bytes;
  auto set = pe ? qld_set_pe_options : qld_set_macho_options;
  const char *inputs[] = {argv[2]};
  qld_options_t opts;
  qld_options_init(&opts);
  opts.inputs = inputs; opts.n_inputs = 1; opts.mode = QLD_MODE_SHARED;
  qld_elf_options_t config;
  qld_elf_options_init(&config);
  config.linker = argv[3];
  const char *peArgs[] = {"/noentry"};
  const char *machArgs[] = {"-arch", "x86_64", "-platform_version", "macos", "11.0", "11.0"};
  config.arguments = pe ? peArgs : machArgs;
  config.n_arguments = pe ? 1 : 6;
  uint8_t *bytes = nullptr;
  size_t size = 0;
  char *error = nullptr;
  int rc = link(&opts, &config, &bytes, &size, &error);
  require(rc == QLD_OK, error); image(bytes, size, pe); qld_free_bytes(bytes, size);
  config.struct_size = 0;
  bytes = reinterpret_cast<uint8_t *>(1); size = 42;
  require(link(&opts, &config, &bytes, &size, &error) == QLD_INVALID_ARGUMENT, "invalid config accepted");
  require(!bytes && !size && error, "failure outputs not cleared");
  qld_free_string(error); error = nullptr;
  config.struct_size = sizeof(config);
  auto saved = config.arguments;
  config.arguments = nullptr;
  require(link(&opts, &config, &bytes, &size, &error) == QLD_INVALID_ARGUMENT, "null arguments accepted");
  qld_free_string(error); error = nullptr; config.arguments = saved;
  qld::Options cpp;
  cpp.inputs = {argv[2]}; cpp.mode = qld::Mode::Shared;
  auto& backend = pe ? cpp.pe : cpp.macho;
  backend.linker = argv[3];
  for (size_t i = 0; i < config.n_arguments; ++i) backend.arguments.emplace_back(config.arguments[i]);
  auto result = qld::link(cpp);
  require(result.ok(), result.error().message.c_str());
  image(result->bytes.data(), result->bytes.size(), pe);
  qld::Linker builder;
  std::string message;
  require(builder.addInput(argv[2], message), message.c_str());
  builder.setMode(qld::Mode::Shared);
  auto updated = pe ? builder.setPeOptions(backend) : builder.setMachOOptions(backend);
  require(updated.ok(), updated.error().message.c_str());
  backend.arguments.clear(); backend.linker.clear(); // Builder owns a copy.
  result = builder.run();
  require(result.ok(), result.error().message.c_str()); image(result->bytes.data(), result->bytes.size(), pe);
  config.struct_size = 0;
  require(set(builder.native_handle(), &config, &error) == QLD_INVALID_ARGUMENT, "bad setter accepted");
  qld_free_string(error); error = nullptr;
  require(builder.run().ok(), "invalid update damaged builder");
  cpp.pe.enabled = cpp.macho.enabled = true;
  require(!qld::link(cpp), "conflicting backends accepted");
  std::cout << "cross-format C/C++ APIs passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n'; return 1;
}
