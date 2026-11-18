#include "ld.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

void dynamicImage(const uint8_t* bytes, size_t size) {
  require(bytes && size >= 64, "missing native dynamic image");
  require(!std::memcmp(bytes, "\177ELF", 4), "native image is not ELF");
  require(bytes[4] == 2 && bytes[5] == 1, "expected ELF64 little-endian");
  require(bytes[16] == 3 && bytes[17] == 0, "expected native ET_DYN");
}
}  // namespace

int main(int argc, char** argv) try {
  require(argc == 2, "expected one PIC object");
  const char* inputs[] = {argv[1]};
  qld_options_t options;
  qld_options_init(&options);
  options.inputs = inputs;
  options.n_inputs = 1;
  options.mode = QLD_MODE_SHARED;
  qld_elf_options_t elf;
  qld_elf_options_init(&elf);
  const char* arguments[] = {"--soname=api-native.so", "--hash-style=both"};
  elf.arguments = arguments;
  elf.n_arguments = 2;
  uint8_t* bytes = nullptr;
  size_t size = 0;
  char* error = nullptr;
  int status =
      qld_link_elf_bytes(&options, &elf, &bytes, &size, &error);
  std::string message = error ? error : "native C dynamic link failed";
  qld_free_string(error);
  require(status == QLD_OK, message);
  dynamicImage(bytes, size);
  qld_free_bytes(bytes, size);

  qld::Options cpp;
  cpp.inputs = {argv[1]};
  cpp.mode = qld::Mode::Shared;
  cpp.elf.enabled = true;  // NULL executable still means native.
  cpp.elf.arguments = {"--soname=cpp-native.so"};
  auto result = qld::link(cpp);
  require(result.ok(), result.ok() ? "" : result.error().message);
  dynamicImage(result->bytes.data(), result->bytes.size());

  qld::Linker builder;
  std::string diagnostic;
  require(builder.addInput(argv[1], diagnostic), diagnostic);
  builder.setMode(qld::Mode::Shared);
  qld::ElfOptions native;
  native.enabled = true;
  native.arguments = {"--soname=builder-native.so"};
  auto configured = builder.setElfOptions(native);
  require(configured.ok(),
          configured.ok() ? "" : configured.error().message);
  auto built = builder.run();
  require(built.ok(), built.ok() ? "" : built.error().message);
  dynamicImage(built->bytes.data(), built->bytes.size());
  std::cout << "native dynamic C/C++ APIs passed\n";
  return 0;
} catch (const std::exception& exception) {
  std::cerr << exception.what() << '\n';
  return 1;
}
