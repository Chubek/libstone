/*
 * Internal open-addressing hash map from addresses to word-sized values,
 * replacing std::unordered_map<Address, size_t> / std::unordered_set.
 * Linear probing with backward-shift deletion (no tombstones).
 */
#ifndef MEMTKX_INTERNAL_ADDR_MAP_H
#define MEMTKX_INTERNAL_ADDR_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "memtkx/core/base.h"

typedef struct mtkx__addr_map {
  uintptr_t *keys;
  uintptr_t *values;
  unsigned char *used;
  size_t capacity; /* zero or a power of two */
  size_t count;
} mtkx__addr_map;

#define MTKX__ADDR_MAP_INIT {NULL, NULL, NULL, 0, 0}

void mtkx__addr_map_init(mtkx__addr_map *map);
void mtkx__addr_map_destroy(mtkx__addr_map *map);
void mtkx__addr_map_clear(mtkx__addr_map *map);
mtkx_status mtkx__addr_map_reserve(mtkx__addr_map *map, size_t count);

/* Lookup; returns true and stores the value (if out != NULL) when found. */
bool mtkx__addr_map_get(const mtkx__addr_map *map, uintptr_t key, uintptr_t *out);
bool mtkx__addr_map_contains(const mtkx__addr_map *map, uintptr_t key);

/*
 * Inserts key->value when the key is absent (std::unordered_map::emplace
 * semantics). *inserted reports whether a new entry was created.
 */
mtkx_status mtkx__addr_map_emplace(mtkx__addr_map *map, uintptr_t key,
                                   uintptr_t value, bool *inserted);

/* Inserts or overwrites. */
mtkx_status mtkx__addr_map_put(mtkx__addr_map *map, uintptr_t key, uintptr_t value);

/* Removes key; returns true when an entry was removed. */
bool mtkx__addr_map_remove(mtkx__addr_map *map, uintptr_t key);

/* Swaps two maps in constant time. */
void mtkx__addr_map_swap(mtkx__addr_map *a, mtkx__addr_map *b);

/* Iteration: for (i = 0; i < map->capacity; ++i) if (map->used[i]) ... */

#endif /* MEMTKX_INTERNAL_ADDR_MAP_H */
