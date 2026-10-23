/* C11 utilities for the ArchBFD application ABI. */
#include "BFDBinABI.h"

#include <string.h>

uint32_t archbfd_abi_version(void) { return ARCHBFD_ABI_VERSION; }

void archbfd_error_clear(archbfd_error *error) {
  if (error) memset(error, 0, sizeof(*error));
}

const char *archbfd_status_name(archbfd_status status) {
  static const char *const names[] = {
      "ok", "invalid argument", "not found", "I/O error", "unknown format",
      "ambiguous format", "malformed input", "unsupported", "out of range",
      "out of memory", "internal error"};
  return status < sizeof(names) / sizeof(names[0]) ? names[status] : "unknown error";
}

const char *archbfd_format_name(uint32_t format) {
  static const char *const names[] = {"unknown", "ELF", "PE", "COFF", "Mach-O"};
  return format < sizeof(names) / sizeof(names[0]) ? names[format] : "unknown";
}

const char *archbfd_arch_name(uint32_t arch) {
  static const char *const names[] = {
      "unknown", "x86", "x86_64", "arm", "aarch64", "riscv32", "riscv64",
      "powerpc", "powerpc64", "mips", "mips64", "sparc", "sparc64", "s390x",
      "loongarch64", "wasm"};
  return arch < sizeof(names) / sizeof(names[0]) ? names[arch] : "unknown";
}

static archbfd_status fail(archbfd_error *error, archbfd_status code) {
  if (error) {
    archbfd_error_clear(error);
    error->code = code;
    /* All status-name literals fit in message. */
    strcpy(error->message, archbfd_status_name(code));
  }
  return code;
}

static archbfd_status check_range(const void *data, size_t size, uint64_t offset,
                                 uint32_t width, uint32_t endian,
                                 archbfd_error *error) {
  if ((!data && size) || (width != 1 && width != 2 && width != 4 && width != 8) ||
      (endian != ARCHBFD_LITTLE_ENDIAN && endian != ARCHBFD_BIG_ENDIAN))
    return fail(error, ARCHBFD_INVALID_ARGUMENT);
  if (offset > size || width > size - (size_t)offset)
    return fail(error, ARCHBFD_OUT_OF_RANGE);
  return ARCHBFD_OK;
}

archbfd_status archbfd_read_uint(const void *data, size_t size, uint64_t offset,
                                uint32_t width, uint32_t endian, uint64_t *out,
                                archbfd_error *error) {
  uint64_t value = 0;
  uint32_t i;
  archbfd_status status;
  if (!out) return fail(error, ARCHBFD_INVALID_ARGUMENT);
  status = check_range(data, size, offset, width, endian, error);
  if (status != ARCHBFD_OK) return status;
  for (i = 0; i < width; ++i) {
    uint32_t shift = 8 * (endian == ARCHBFD_BIG_ENDIAN ? width - i - 1 : i);
    value |= (uint64_t)((const uint8_t *)data)[(size_t)offset + i] << shift;
  }
  *out = value;
  archbfd_error_clear(error);
  return ARCHBFD_OK;
}

archbfd_status archbfd_write_uint(void *data, size_t size, uint64_t offset,
                                 uint32_t width, uint32_t endian, uint64_t value,
                                 archbfd_error *error) {
  uint32_t i;
  archbfd_status status = check_range(data, size, offset, width, endian, error);
  if (status != ARCHBFD_OK) return status;
  if (width < 8 && value >= (UINT64_C(1) << (width * 8)))
    return fail(error, ARCHBFD_OUT_OF_RANGE);
  for (i = 0; i < width; ++i) {
    uint32_t shift = 8 * (endian == ARCHBFD_BIG_ENDIAN ? width - i - 1 : i);
    ((uint8_t *)data)[(size_t)offset + i] = (uint8_t)(value >> shift);
  }
  archbfd_error_clear(error);
  return ARCHBFD_OK;
}
