#include "internal/addr_map.h"

#include <string.h>

#include "internal/common.h"

static size_t mtkx__hash(uintptr_t key, size_t mask) {
  /* splitmix64 finaliser: object addresses are aligned, so their low bits
   * carry little entropy; mixing spreads them over the table. */
  uint64_t x = (uint64_t)key;
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return (size_t)x & mask;
}

void mtkx__addr_map_init(mtkx__addr_map *map) {
  map->keys = NULL;
  map->values = NULL;
  map->used = NULL;
  map->capacity = 0;
  map->count = 0;
}

void mtkx__addr_map_destroy(mtkx__addr_map *map) {
  mtkx_free(map->keys);
  mtkx_free(map->values);
  mtkx_free(map->used);
  mtkx__addr_map_init(map);
}

void mtkx__addr_map_clear(mtkx__addr_map *map) {
  if (map->used != NULL) {
    memset(map->used, 0, map->capacity);
  }
  map->count = 0;
}

static size_t mtkx__find_slot(const mtkx__addr_map *map, uintptr_t key, bool *found) {
  size_t mask = map->capacity - 1;
  size_t i = mtkx__hash(key, mask);
  while (map->used[i]) {
    if (map->keys[i] == key) {
      *found = true;
      return i;
    }
    i = (i + 1) & mask;
  }
  *found = false;
  return i;
}

static mtkx_status mtkx__rehash(mtkx__addr_map *map, size_t new_capacity) {
  mtkx__addr_map grown;
  size_t i;
  size_t bytes;
  if (!mtkx__mul_size(new_capacity, sizeof(uintptr_t), &bytes)) {
    return MTKX_ERR_NOMEM;
  }
  grown.capacity = new_capacity;
  grown.count = 0;
  grown.keys = (uintptr_t *)mtkx_malloc(bytes);
  grown.values = (uintptr_t *)mtkx_malloc(bytes);
  grown.used = (unsigned char *)mtkx_calloc(new_capacity, 1);
  if (grown.keys == NULL || grown.values == NULL || grown.used == NULL) {
    mtkx_free(grown.keys);
    mtkx_free(grown.values);
    mtkx_free(grown.used);
    return MTKX_ERR_NOMEM;
  }
  for (i = 0; i < map->capacity; ++i) {
    if (map->used[i]) {
      bool found;
      size_t slot = mtkx__find_slot(&grown, map->keys[i], &found);
      grown.used[slot] = 1;
      grown.keys[slot] = map->keys[i];
      grown.values[slot] = map->values[i];
      ++grown.count;
    }
  }
  mtkx__addr_map_destroy(map);
  *map = grown;
  return MTKX_OK;
}

mtkx_status mtkx__addr_map_reserve(mtkx__addr_map *map, size_t count) {
  size_t needed = 16;
  /* keep the load factor at or below 3/4 */
  while (needed - needed / 4 < count) {
    if (needed > SIZE_MAX / 2) {
      return MTKX_ERR_NOMEM;
    }
    needed *= 2;
  }
  if (needed <= map->capacity) {
    return MTKX_OK;
  }
  return mtkx__rehash(map, needed);
}

bool mtkx__addr_map_get(const mtkx__addr_map *map, uintptr_t key, uintptr_t *out) {
  bool found;
  size_t slot;
  if (map->count == 0) {
    return false;
  }
  slot = mtkx__find_slot(map, key, &found);
  if (found && out != NULL) {
    *out = map->values[slot];
  }
  return found;
}

bool mtkx__addr_map_contains(const mtkx__addr_map *map, uintptr_t key) {
  return mtkx__addr_map_get(map, key, NULL);
}

mtkx_status mtkx__addr_map_emplace(mtkx__addr_map *map, uintptr_t key,
                                   uintptr_t value, bool *inserted) {
  bool found = false;
  size_t slot;
  mtkx_status status;
  if (inserted != NULL) {
    *inserted = false;
  }
  if (map->count != 0) {
    slot = mtkx__find_slot(map, key, &found);
    if (found) {
      return MTKX_OK;
    }
  }
  status = mtkx__addr_map_reserve(map, map->count + 1);
  if (status != MTKX_OK) {
    return status;
  }
  slot = mtkx__find_slot(map, key, &found);
  map->used[slot] = 1;
  map->keys[slot] = key;
  map->values[slot] = value;
  ++map->count;
  if (inserted != NULL) {
    *inserted = true;
  }
  return MTKX_OK;
}

mtkx_status mtkx__addr_map_put(mtkx__addr_map *map, uintptr_t key, uintptr_t value) {
  bool found = false;
  size_t slot;
  if (map->count != 0) {
    slot = mtkx__find_slot(map, key, &found);
    if (found) {
      map->values[slot] = value;
      return MTKX_OK;
    }
  }
  return mtkx__addr_map_emplace(map, key, value, NULL);
}

bool mtkx__addr_map_remove(mtkx__addr_map *map, uintptr_t key) {
  bool found;
  size_t mask;
  size_t hole;
  size_t next;
  if (map->count == 0) {
    return false;
  }
  hole = mtkx__find_slot(map, key, &found);
  if (!found) {
    return false;
  }
  mask = map->capacity - 1;
  map->used[hole] = 0;
  --map->count;
  /* backward-shift deletion keeps every probe chain contiguous */
  next = (hole + 1) & mask;
  while (map->used[next]) {
    size_t ideal = mtkx__hash(map->keys[next], mask);
    /* distance from the entry's ideal slot to its current and to the hole */
    size_t dist_next = (next - ideal) & mask;
    size_t dist_hole = (hole - ideal) & mask;
    if (dist_hole <= dist_next) {
      map->keys[hole] = map->keys[next];
      map->values[hole] = map->values[next];
      map->used[hole] = 1;
      map->used[next] = 0;
      hole = next;
    }
    next = (next + 1) & mask;
  }
  return true;
}

void mtkx__addr_map_swap(mtkx__addr_map *a, mtkx__addr_map *b) {
  mtkx__addr_map tmp = *a;
  *a = *b;
  *b = tmp;
}
