#include "ld.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) try {
  if (argc != 4) throw std::runtime_error("expected format and two objects");
  std::string format = argv[1];
  const char *inputs[] = {argv[2], argv[3]};
  qld_options_t options;
  qld_options_init(&options);
  options.inputs = inputs; options.n_inputs = 2;
  options.entry = format == "elf" ? "_start" : format == "pe" ? "entry" : "_entry";
  qld_elf_options_t config;
  qld_elf_options_init(&config); // NULL linker means native, not PATH lookup.
  auto link = format == "elf" ? qld_link_elf_bytes :
              format == "pe" ? qld_link_pe_bytes : qld_link_macho_bytes;
  uint8_t *bytes = nullptr;
  size_t size = 0;
  char *error = nullptr;
  int rc = link(&options, &config, &bytes, &size, &error);
  std::string message = error ? error : "missing image";
  qld_free_string(error);
  bool ok = !rc && bytes && size > 64;
  qld_free_bytes(bytes, size);
  if (!ok) throw std::runtime_error(message);
  qld::Options cpp;
  cpp.inputs = {argv[2], argv[3]}; cpp.entry = options.entry; cpp.hasEntry = true;
  auto result = qld::link(cpp); // Automatic selection must also be native.
  if (!result) throw std::runtime_error(result.error().message);
  qld::Linker builder;
  std::string diagnostic;
  if (!builder.addInput(argv[2], diagnostic) || !builder.addInput(argv[3], diagnostic))
    throw std::runtime_error(diagnostic);
  builder.setEntry(options.entry);
  if (!builder.run()) throw std::runtime_error("native incremental link failed");
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n'; return 1;
}
