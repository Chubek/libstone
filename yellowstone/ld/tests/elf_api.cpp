#include "ld.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message ? message : "failed assertion");
}
static void image(const uint8_t *bytes, size_t size) {
  require(bytes && size > 64 && !std::memcmp(bytes, "\177ELF", 4), "missing ELF bytes");
  require(bytes[16] == 3, "expected shared image");
}

int main(int argc, char **argv) try {
  require(argc == 3, "expected object and qobjld paths");
  const char *inputs[] = {argv[1]};
  qld_options_t opts;
  qld_options_init(&opts);
  opts.inputs = inputs; opts.n_inputs = 1; opts.mode = QLD_MODE_SHARED;
  uint8_t *bytes = nullptr;
  size_t size = 0;
  char *error = nullptr;
  // The original C ABI now also emits real DSOs, with no required new fields.
  int rc = qld_link_bytes(&opts, &bytes, &size, &error);
  require(rc == 0, error); image(bytes, size); qld_free_bytes(bytes, size);
  qld_elf_options_t elf;
  qld_elf_options_init(&elf);
  const char *args[] = {"--soname=api.so", "--hash-style=both"};
  elf.arguments = args; elf.n_arguments = 2;
  rc = qld_link_elf_bytes(&opts, &elf, &bytes, &size, &error);
  require(rc == 0, error); image(bytes, size); qld_free_bytes(bytes, size);
  elf.struct_size = 0;
  bytes = reinterpret_cast<uint8_t *>(1); size = 42;
  require(qld_link_elf_bytes(&opts, &elf, &bytes, &size, &error) == QLD_INVALID_ARGUMENT,
          "invalid options accepted");
  require(!bytes && !size && error, "failure did not clear outputs");
  qld_free_string(error); error = nullptr;

  qld::Linker linker;
  std::string message;
  require(linker.addInput(argv[1], message), message.c_str());
  linker.setMode(qld::Mode::Shared);
  {
    qld::ElfOptions config;
    config.arguments = {"--soname=builder.so", "--hash-style=both"};
    auto result = linker.setElfOptions(config);
    require(result.ok(), result.error().message.c_str());
  } // Builder owns its settings, including each argument.
  int calls = 0;
  auto directive = [](void *opaque, const char *const *, size_t, qld_source_sink sink,
                       void *out, char *, size_t) -> int {
    ++*static_cast<int *>(opaque);
    const char *text = "SECTIONS { .sdk : { LONG(37) } } INSERT AFTER .data;\n";
    return sink(out, text, std::strlen(text));
  };
  rc = qld_register_directive(linker.native_handle(), "extra", directive, &calls, &error);
  require(rc == 0, error);
  linker.setScriptText("@extra\n");
  for (int i = 1; i <= 2; ++i) {
    auto result = linker.run();
    require(result.ok(), result.error().message.c_str());
    image(result->bytes.data(), result->bytes.size());
    require(calls == i, "SDK did not execute exactly once per invocation");
  }
  // Invalid updates preserve the preceding working configuration.
  require(qld_set_elf_options(linker.native_handle(), &elf, &error) == QLD_INVALID_ARGUMENT,
          "bad builder settings accepted");
  qld_free_string(error); error = nullptr;
  require(linker.run().ok(), "invalid update damaged builder");
  qld::Options cpp;
  cpp.inputs = {argv[1]}; cpp.mode = qld::Mode::Shared;
  cpp.elf.arguments = {"--soname=cpp.so"};
  auto result = qld::link(cpp);
  require(result.ok(), result.error().message.c_str());
  image(result->bytes.data(), result->bytes.size());
  std::cout << "ELF C/C++ API checks passed\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
  return 1;
}
