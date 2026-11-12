/**
 * @file memtkx/core/types.h
 * @brief Address vocabulary, half-open address ranges and alignment helpers.
 */
#ifndef MEMTKX_CORE_TYPES_H
#define MEMTKX_CORE_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "memtkx/core/base.h"

MTKX_BEGIN_DECLS

/** An address as an unsigned integer. */
typedef uintptr_t mtkx_address;
/** The address of a managed object. */
typedef uintptr_t mtkx_object_reference;
/** A machine word. */
typedef uintptr_t mtkx_word;
/** A signed distance between addresses. */
typedef ptrdiff_t mtkx_offset;
/** A raw storage byte. */
typedef unsigned char mtkx_byte;

/** Size of a machine word in bytes. */
#define MTKX_WORD_BYTES (sizeof(mtkx_word))

/* ------------------------------------------------------------------ */
/*  Alignment queries                                                 */
/* ------------------------------------------------------------------ */

/** @internal Union of the most strictly aligned scalar types. */
union mtkx_max_align_u_ {
  long double ld;
  long long ll;
  double d;
  void *p;
  void (*fp)(void);
};
/** @internal Probe used to compute MTKX_MAX_ALIGN portably in C99. */
struct mtkx_max_align_probe_ {
  char c;
  union mtkx_max_align_u_ u;
};

/**
 * @brief The fundamental alignment, equivalent to alignof(max_align_t).
 *
 * C99 has no max_align_t; this computes the alignment of the most
 * strictly aligned scalar type and is a constant expression.
 */
#define MTKX_MAX_ALIGN (offsetof(struct mtkx_max_align_probe_, u))

/**
 * @brief Alignment of type @p T as a size_t constant.
 *
 * Uses alignof in C++, _Alignof in C11 and an offsetof probe in C99.
 */
#if defined(__cplusplus)
#define MTKX_ALIGNOF(T) (alignof(T))
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define MTKX_ALIGNOF(T) ((size_t)_Alignof(T))
#else
/* struct { char; T; } is exactly alignof(T) + sizeof(T) bytes. */
#define MTKX_ALIGNOF(T)                                                       \
  (sizeof(struct { char mtkx_c_; T mtkx_t_; }) - sizeof(T))
#endif

/* ------------------------------------------------------------------ */
/*  Pointer <-> address conversion                                    */
/* ------------------------------------------------------------------ */

/** Converts a pointer to an address. Does not allocate or check anything. */
#define mtkx_as_address(ptr) ((mtkx_address)(const void *)(ptr))
/** Converts an address back to a `T *`. Does not construct or check anything. */
#define mtkx_from_address(T, addr) ((T *)(void *)(uintptr_t)(addr))

/* ------------------------------------------------------------------ */
/*  Address ranges                                                    */
/* ------------------------------------------------------------------ */

/** A half-open address range `[start, end)`. */
typedef struct mtkx_address_range {
  mtkx_address start;
  mtkx_address end;
} mtkx_address_range;

/** Builds a range value. */
MTKX_INLINE mtkx_address_range mtkx_range(mtkx_address start, mtkx_address end) {
  mtkx_address_range r;
  r.start = start;
  r.end = end;
  return r;
}

/** @return true when the range holds no bytes (start >= end). */
MTKX_INLINE bool mtkx_range_empty(mtkx_address_range r) { return r.start >= r.end; }

/** @return the number of bytes in the range; 0 for empty or reversed ranges. */
MTKX_INLINE size_t mtkx_range_size(mtkx_address_range r) {
  return r.start >= r.end ? 0u : (size_t)(r.end - r.start);
}

/** @return true when @p addr lies in `[start, end)`. */
MTKX_INLINE bool mtkx_range_contains(mtkx_address_range r, mtkx_address addr) {
  return addr >= r.start && addr < r.end;
}

/** @return true when @p other lies entirely inside @p r. */
MTKX_INLINE bool mtkx_range_contains_range(mtkx_address_range r,
                                           mtkx_address_range other) {
  return other.start >= r.start && other.end <= r.end;
}

/** @return true when the ranges share at least one byte. */
MTKX_INLINE bool mtkx_range_overlaps(mtkx_address_range r, mtkx_address_range other) {
  return r.start < other.end && other.start < r.end;
}

/**
 * @brief Clamps @p addr into the range. An address at or above `end`
 *        yields `end`, which is an exclusive boundary.
 */
MTKX_INLINE mtkx_address mtkx_range_clamp(mtkx_address_range r, mtkx_address addr) {
  if (addr < r.start) {
    return r.start;
  }
  if (addr >= r.end) {
    return r.end;
  }
  return addr;
}

/** @return true when both bounds are equal. */
MTKX_INLINE bool mtkx_range_equal(mtkx_address_range a, mtkx_address_range b) {
  return a.start == b.start && a.end == b.end;
}

/* ------------------------------------------------------------------ */
/*  Alignment arithmetic                                              */
/* ------------------------------------------------------------------ */

/** @return true for 1, 2, 4, 8 ...; false for 0. */
MTKX_INLINE bool mtkx_is_power_of_two(size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

/** Rounds @p value up to @p alignment (a power of two; <= 1 is a no-op). */
MTKX_INLINE mtkx_address mtkx_align_up(mtkx_address value, size_t alignment) {
  mtkx_address mask;
  if (alignment <= 1) {
    return value;
  }
  mask = (mtkx_address)(alignment - 1);
  return (value + mask) & ~mask;
}

/** Rounds @p value down to @p alignment (a power of two; <= 1 is a no-op). */
MTKX_INLINE mtkx_address mtkx_align_down(mtkx_address value, size_t alignment) {
  mtkx_address mask;
  if (alignment <= 1) {
    return value;
  }
  mask = (mtkx_address)(alignment - 1);
  return value & ~mask;
}

/**
 * @return true when @p bytes fit between `align_up(start, alignment)`
 *         and @p limit.
 */
MTKX_INLINE bool mtkx_can_allocate(mtkx_address start, mtkx_address limit,
                                   size_t bytes, size_t alignment) {
  mtkx_address aligned;
  if (start >= limit) {
    return false;
  }
  aligned = mtkx_align_up(start, alignment);
  return aligned >= start && aligned <= limit &&
         (size_t)(limit - aligned) >= bytes;
}

MTKX_END_DECLS

#endif /* MEMTKX_CORE_TYPES_H */
