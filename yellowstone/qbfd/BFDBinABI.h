/* ArchBFD ABI v1: native application records, not packed on-disk structs.
 * Independent of GNU libbfd. See ArchBFD.md for ownership and error contracts. */
#ifndef ARCHBFD_BIN_ABI_H
#define ARCHBFD_BIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(ARCHBFD_SHARED)
# ifdef ARCHBFD_BUILDING
#  define ARCHBFD_API __declspec(dllexport)
# else
#  define ARCHBFD_API __declspec(dllimport)
# endif
#elif defined(__GNUC__) && defined(ARCHBFD_SHARED)
# define ARCHBFD_API __attribute__((visibility("default")))
#else
# define ARCHBFD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHBFD_ABI_VERSION 1u
#define ARCHBFD_NO_INDEX UINT32_MAX

typedef uint32_t archbfd_status;
enum {
  ARCHBFD_OK = 0, ARCHBFD_INVALID_ARGUMENT = 1, ARCHBFD_NOT_FOUND = 2,
  ARCHBFD_IO_ERROR = 3, ARCHBFD_UNKNOWN_FORMAT = 4, ARCHBFD_AMBIGUOUS = 5,
  ARCHBFD_MALFORMED = 6, ARCHBFD_UNSUPPORTED = 7, ARCHBFD_OUT_OF_RANGE = 8,
  ARCHBFD_NO_MEMORY = 9, ARCHBFD_INTERNAL_ERROR = 10
};
enum {
  ARCHBFD_FORMAT_UNKNOWN = 0, ARCHBFD_FORMAT_ELF = 1, ARCHBFD_FORMAT_PE = 2,
  ARCHBFD_FORMAT_COFF = 3, ARCHBFD_FORMAT_MACHO = 4
};
enum {
  ARCHBFD_ARCH_UNKNOWN = 0, ARCHBFD_ARCH_X86 = 1, ARCHBFD_ARCH_X86_64 = 2,
  ARCHBFD_ARCH_ARM = 3, ARCHBFD_ARCH_AARCH64 = 4, ARCHBFD_ARCH_RISCV32 = 5,
  ARCHBFD_ARCH_RISCV64 = 6, ARCHBFD_ARCH_POWERPC = 7, ARCHBFD_ARCH_POWERPC64 = 8,
  ARCHBFD_ARCH_MIPS = 9, ARCHBFD_ARCH_MIPS64 = 10, ARCHBFD_ARCH_SPARC = 11,
  ARCHBFD_ARCH_SPARC64 = 12, ARCHBFD_ARCH_S390X = 13,
  ARCHBFD_ARCH_LOONGARCH64 = 14, ARCHBFD_ARCH_WEBASSEMBLY = 15
};
enum { ARCHBFD_LITTLE_ENDIAN = 0, ARCHBFD_BIG_ENDIAN = 1 };
enum {
  ARCHBFD_FILE_UNKNOWN = 0, ARCHBFD_FILE_RELOCATABLE = 1,
  ARCHBFD_FILE_EXECUTABLE = 2, ARCHBFD_FILE_SHARED = 3, ARCHBFD_FILE_CORE = 4
};
enum {
  ARCHBFD_SEC_ALLOC = 1u << 0, ARCHBFD_SEC_LOAD = 1u << 1,
  ARCHBFD_SEC_CODE = 1u << 2, ARCHBFD_SEC_DATA = 1u << 3,
  ARCHBFD_SEC_READ_ONLY = 1u << 4, ARCHBFD_SEC_WRITABLE = 1u << 5,
  ARCHBFD_SEC_EXEC = 1u << 6, ARCHBFD_SEC_BSS = 1u << 7,
  ARCHBFD_SEC_DEBUG = 1u << 8, ARCHBFD_SEC_RELOC = 1u << 9,
  ARCHBFD_SEC_STRINGS = 1u << 10, ARCHBFD_SEC_NOTE = 1u << 11,
  ARCHBFD_SEC_TLS = 1u << 12, ARCHBFD_SEC_DISCARD = 1u << 13
};
enum {
  ARCHBFD_SYM_UNDEFINED = 1u << 0, ARCHBFD_SYM_ABSOLUTE = 1u << 1,
  ARCHBFD_SYM_HIDDEN = 1u << 2, ARCHBFD_SYM_EXPORTED = 1u << 3,
  ARCHBFD_SYM_IMPORTED = 1u << 4, ARCHBFD_SYM_SYNTHETIC = 1u << 5,
  ARCHBFD_SYM_DEBUG = 1u << 6
};
enum {
  ARCHBFD_BIND_LOCAL = 0, ARCHBFD_BIND_GLOBAL = 1,
  ARCHBFD_BIND_WEAK = 2, ARCHBFD_BIND_UNIQUE = 3
};
enum {
  ARCHBFD_SYMBOL_NONE = 0, ARCHBFD_SYMBOL_OBJECT = 1, ARCHBFD_SYMBOL_FUNCTION = 2,
  ARCHBFD_SYMBOL_SECTION = 3, ARCHBFD_SYMBOL_FILE = 4, ARCHBFD_SYMBOL_COMMON = 5,
  ARCHBFD_SYMBOL_TLS = 6, ARCHBFD_SYMBOL_INDIRECT = 7
};
enum { ARCHBFD_STATIC_SYMBOLS = 0, ARCHBFD_DYNAMIC_SYMBOLS = 1 };
enum { ARCHBFD_SEG_READ = 1u, ARCHBFD_SEG_WRITE = 2u, ARCHBFD_SEG_EXEC = 4u };

typedef struct archbfd_object archbfd_object;
typedef struct archbfd_archive archbfd_archive;
typedef struct { archbfd_status code; char message[256]; } archbfd_error;
typedef struct { const char *data; size_t size; } archbfd_string;
typedef struct { const uint8_t *data; size_t size; } archbfd_bytes;

typedef struct {
  uint32_t format, arch, endian, file_type, address_bits;
  uint64_t entry_point, image_base, file_size;
  size_t section_count, symbol_count, dynamic_symbol_count, segment_count;
  size_t dependency_count;
  archbfd_string target;
} archbfd_object_info;

typedef struct {
  uint32_t index, flags, native_type;
  uint64_t native_flags, vma, lma, size, file_offset, file_size, alignment;
  archbfd_string name;
} archbfd_section;

typedef struct {
  uint32_t index, section, binding, kind, flags, native_type;
  uint64_t value, size;
  archbfd_string name;
} archbfd_symbol;

typedef struct {
  uint64_t offset, native_value;
  int64_t addend;
  uint32_t section, symbol, target_section, native_type, bit_size;
  uint32_t pc_relative, has_addend, dynamic_symbol, scattered;
  archbfd_string type_name;
} archbfd_relocation;

typedef struct {
  uint32_t flags, native_type;
  uint64_t vaddr, paddr, file_offset, file_size, memory_size, alignment;
  archbfd_string name;
} archbfd_segment;

typedef struct {
  uint64_t header_offset, file_offset, size, mtime;
  uint32_t mode, uid, gid, special;
  archbfd_string name;
} archbfd_member;

typedef struct {
  uint32_t cpu_type, cpu_subtype, alignment_power;
  uint64_t offset, size;
} archbfd_macho_slice;

/* error is optional. Every status-returning call clears it on success.
 * Output records are unchanged on failure; open calls instead set *out = NULL.
 * Input byte ranges may be NULL only when size is zero. */
ARCHBFD_API uint32_t archbfd_abi_version(void);
ARCHBFD_API const char *archbfd_status_name(archbfd_status status);
ARCHBFD_API const char *archbfd_format_name(uint32_t format);
ARCHBFD_API const char *archbfd_arch_name(uint32_t arch);
ARCHBFD_API void archbfd_error_clear(archbfd_error *error);

/* Checked, unaligned integer access in file octets. width is 1, 2, 4 or 8.
 * Writes reject values that do not fit and never modify bytes on failure. */
ARCHBFD_API archbfd_status archbfd_read_uint(const void *data, size_t size,
    uint64_t offset, uint32_t width, uint32_t endian, uint64_t *out,
    archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_write_uint(void *data, size_t size,
    uint64_t offset, uint32_t width, uint32_t endian, uint64_t value,
    archbfd_error *error);

/* target may be NULL for detection. Memory opens copy all input bytes.
 * File paths use the platform's native narrow encoding. */
ARCHBFD_API archbfd_status archbfd_open_file(const char *path, const char *target,
    archbfd_object **out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_open_memory(const void *data, size_t size,
    const char *target, archbfd_object **out, archbfd_error *error);
/* Select a universal Mach-O slice explicitly; the returned object owns a copy. */
ARCHBFD_API archbfd_status archbfd_macho_slice_count(const void *data, size_t size,
    size_t *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_get_macho_slice(const void *data, size_t size,
    size_t slot, archbfd_macho_slice *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_open_macho_slice(const void *data, size_t size,
    size_t slice, archbfd_object **out, archbfd_error *error);
ARCHBFD_API void archbfd_close(archbfd_object *object);

/* Returned strings and bytes are borrowed until close; names are length-based.
 * Slots are zero-based. ELF retains its null section/symbol entries.
 * Missing section/symbol references use ARCHBFD_NO_INDEX. */
ARCHBFD_API archbfd_status archbfd_get_object_info(const archbfd_object *object,
    archbfd_object_info *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_get_bytes(const archbfd_object *object,
    archbfd_bytes *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_get_section(const archbfd_object *object,
    size_t slot, archbfd_section *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_get_section_bytes(const archbfd_object *object,
    size_t slot, archbfd_bytes *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_get_symbol(const archbfd_object *object,
    uint32_t table, size_t slot, archbfd_symbol *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_get_relocation_count(const archbfd_object *object,
    size_t section, size_t *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_get_relocation(const archbfd_object *object,
    size_t section, size_t slot, archbfd_relocation *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_get_segment(const archbfd_object *object,
    size_t slot, archbfd_segment *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_get_dependency(const archbfd_object *object,
    size_t slot, archbfd_string *out, archbfd_error *error);
/* Section-based VA lookup: rejects BSS and ranges beyond file-backed bytes. */
ARCHBFD_API archbfd_status archbfd_bytes_at_address(const archbfd_object *object,
    uint64_t address, uint64_t size, archbfd_bytes *out, archbfd_error *error);

/* Archive members opened as objects are independent of the archive lifetime.
 * label supplies a base path for thin members; NULL uses the working directory.
 * Archive access must be externally serialized (thin members are cached). */
ARCHBFD_API archbfd_status archbfd_archive_open_file(const char *path,
    archbfd_archive **out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_archive_open_memory(const void *data, size_t size,
    const char *label, archbfd_archive **out, archbfd_error *error);
ARCHBFD_API void archbfd_archive_close(archbfd_archive *archive);
ARCHBFD_API archbfd_status archbfd_archive_member_count(const archbfd_archive *archive,
    size_t *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_archive_get_member(const archbfd_archive *archive,
    size_t slot, archbfd_member *out, archbfd_error *error);
ARCHBFD_API archbfd_status archbfd_archive_open_member(archbfd_archive *archive,
    size_t slot, archbfd_object **out, archbfd_error *error);

#ifdef __cplusplus
}
#endif
#endif
