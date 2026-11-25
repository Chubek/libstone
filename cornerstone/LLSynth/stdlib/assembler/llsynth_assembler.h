#ifndef LLSYNTH_ASSEMBLER_H
#define LLSYNTH_ASSEMBLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(LLSYNTH_ASSEMBLER_EXPORTS)
#define LLSYNTH_ASSEMBLER_API __declspec(dllexport)
#else
#define LLSYNTH_ASSEMBLER_API
#endif

typedef struct llsynth_assembler_result {
    const uint8_t *bytes;
    size_t size;
    void *owner;
} llsynth_assembler_result;

/* Open a fresh context, assemble once, and return a borrowed flat image.
 * selector is a bank path unless use_target is nonzero. An empty selector
 * means the default target, and empty syntax keeps the context's default.
 * Pass a fresh result; it is zeroed on failure. Free every successful result
 * with llsynth_assembler_release(), including empty results. */
LLSYNTH_ASSEMBLER_API int llsynth_assembler_flat(
    const char *source, const char *selector, const char *syntax, int use_target,
    llsynth_assembler_result *result, char *error, size_t error_size);
LLSYNTH_ASSEMBLER_API void llsynth_assembler_release(llsynth_assembler_result *result);

#undef LLSYNTH_ASSEMBLER_API

#ifdef __cplusplus
}
#endif
#endif
