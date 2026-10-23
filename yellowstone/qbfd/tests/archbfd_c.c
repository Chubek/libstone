/* Compiled as C, then linked with the C++ regression driver. */
#include "ArchBFD.h"
#include <string.h>

#define REQUIRE(c) do { if (!(c)) return __LINE__; } while (0)

int archbfd_test_c(const void *fixture, size_t size) {
  archbfd_object *file = NULL;
  archbfd_error error;
  archbfd_object_info info;
  archbfd_section section;
  archbfd_symbol symbol;
  archbfd_relocation relocation;
  archbfd_bytes bytes;
  size_t count;
  uint8_t buffer[10] = {0};
  uint8_t original[10];
  uint64_t value = 123;
  REQUIRE(archbfd_abi_version() == ARCHBFD_ABI_VERSION);
  REQUIRE(archbfd_write_uint(buffer, sizeof(buffer), 1, 8, ARCHBFD_BIG_ENDIAN,
                            UINT64_C(0x0123456789abcdef), &error) == ARCHBFD_OK);
  REQUIRE(buffer[1] == 1 && buffer[8] == 0xef && buffer[0] == 0 && buffer[9] == 0);
  REQUIRE(archbfd_read_uint(buffer, sizeof(buffer), 1, 8, ARCHBFD_BIG_ENDIAN,
                           &value, &error) == ARCHBFD_OK);
  REQUIRE(value == UINT64_C(0x0123456789abcdef));
  REQUIRE(archbfd_read_uint(buffer, sizeof(buffer), 1, 8, ARCHBFD_LITTLE_ENDIAN,
                           &value, &error) == ARCHBFD_OK);
  REQUIRE(value == UINT64_C(0xefcdab8967452301));
  memcpy(original, buffer, sizeof(buffer));
  REQUIRE(archbfd_write_uint(buffer, sizeof(buffer), 1, 1, ARCHBFD_LITTLE_ENDIAN,
                            256, &error) == ARCHBFD_OUT_OF_RANGE);
  REQUIRE(memcmp(original, buffer, sizeof(buffer)) == 0);
  REQUIRE(archbfd_read_uint(buffer, sizeof(buffer), UINT64_MAX, 8, 0,
                           &value, &error) == ARCHBFD_OUT_OF_RANGE);
  REQUIRE(value == UINT64_C(0xefcdab8967452301));
  REQUIRE(archbfd_read_uint(NULL, 1, 0, 1, 0, &value, NULL) == ARCHBFD_INVALID_ARGUMENT);
  REQUIRE(archbfd_read_uint(NULL, 0, 0, 1, 0, &value, NULL) == ARCHBFD_OUT_OF_RANGE);
  REQUIRE(archbfd_read_uint(buffer, sizeof(buffer), 0, 3, 0, &value, NULL) == ARCHBFD_INVALID_ARGUMENT);
  REQUIRE(archbfd_read_uint(buffer, sizeof(buffer), 0, 1, 2, &value, NULL) == ARCHBFD_INVALID_ARGUMENT);

  REQUIRE(archbfd_open_memory(fixture, size, NULL, &file, &error) == ARCHBFD_OK);
  REQUIRE(file != NULL && error.code == ARCHBFD_OK && error.message[0] == 0);
  REQUIRE(archbfd_get_object_info(file, &info, &error) == ARCHBFD_OK);
  REQUIRE(info.format == ARCHBFD_FORMAT_COFF && info.arch == ARCHBFD_ARCH_X86_64);
  REQUIRE(info.address_bits == 64 && info.endian == ARCHBFD_LITTLE_ENDIAN);
  REQUIRE(info.section_count == 2 && info.symbol_count == 1 && info.dynamic_symbol_count == 0);
  REQUIRE(archbfd_get_section(file, 0, &section, &error) == ARCHBFD_OK);
  REQUIRE(section.vma == 0x1000 && section.name.size == 5);
  REQUIRE(memcmp(section.name.data, ".text", 5) == 0);
  REQUIRE((section.flags & ARCHBFD_SEC_CODE) != 0);
  REQUIRE(archbfd_get_section(file, SIZE_MAX, &section, &error) == ARCHBFD_OUT_OF_RANGE);
  REQUIRE(section.vma == 0x1000); /* failure did not modify the record */
  REQUIRE(archbfd_get_symbol(file, ARCHBFD_STATIC_SYMBOLS, 0, &symbol, &error) == ARCHBFD_OK);
  REQUIRE(symbol.binding == ARCHBFD_BIND_GLOBAL && symbol.kind == ARCHBFD_SYMBOL_FUNCTION);
  REQUIRE(symbol.section == 0 && symbol.name.size == 5 && symbol.value == 0x1000);
  REQUIRE(archbfd_get_symbol(file, 42, 0, &symbol, &error) == ARCHBFD_INVALID_ARGUMENT);
  REQUIRE(archbfd_get_symbol(file, ARCHBFD_DYNAMIC_SYMBOLS, 0, &symbol, &error) == ARCHBFD_OUT_OF_RANGE);
  REQUIRE(archbfd_get_relocation_count(file, 0, &count, &error) == ARCHBFD_OK && count == 1);
  REQUIRE(archbfd_get_relocation(file, 0, 0, &relocation, &error) == ARCHBFD_OK);
  REQUIRE(relocation.section == 0 && relocation.symbol == 0 && relocation.offset == 0);
  REQUIRE(relocation.target_section == ARCHBFD_NO_INDEX && relocation.has_addend);
  REQUIRE(relocation.pc_relative && relocation.addend == -4 && relocation.bit_size == 32);
  REQUIRE(archbfd_get_section_bytes(file, 1, &bytes, &error) == ARCHBFD_OK && bytes.size == 0);
  REQUIRE(archbfd_bytes_at_address(file, 0x2000, 1, &bytes, &error) == ARCHBFD_OUT_OF_RANGE);
  REQUIRE(archbfd_bytes_at_address(file, 0x1000, 4, &bytes, &error) == ARCHBFD_OK);
  REQUIRE(bytes.size == 4 && bytes.data[0] == 0xfc);
  REQUIRE(archbfd_bytes_at_address(file, 0x1000, UINT64_MAX, &bytes, &error) == ARCHBFD_OUT_OF_RANGE);
  REQUIRE(bytes.size == 4);
  REQUIRE(archbfd_get_bytes(file, &bytes, &error) == ARCHBFD_OK && bytes.size == size);
  REQUIRE(bytes.data != fixture); /* owned copy, not the caller's buffer */
  archbfd_close(file);
  file = (archbfd_object *)(uintptr_t)1;
  REQUIRE(archbfd_open_memory(NULL, 1, NULL, &file, &error) == ARCHBFD_INVALID_ARGUMENT);
  REQUIRE(file == NULL && error.code == ARCHBFD_INVALID_ARGUMENT);
  REQUIRE(archbfd_open_memory(NULL, 0, NULL, &file, &error) == ARCHBFD_UNKNOWN_FORMAT);
  REQUIRE(file == NULL);
  REQUIRE(archbfd_open_memory(fixture, size, "missing-target", &file, &error) == ARCHBFD_UNSUPPORTED);
  REQUIRE(file == NULL);
  REQUIRE(archbfd_get_object_info(NULL, &info, &error) == ARCHBFD_INVALID_ARGUMENT);
  archbfd_close(NULL);
  archbfd_archive_close(NULL);
  return 0;
}
