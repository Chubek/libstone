// C runtime emitted by the ASDL generator, ported from scripts/asdl.pl.
#pragma once

#include <string_view>

namespace asdl::codegen_detail {
inline constexpr std::string_view common = R"ASDL(#ifndef ASDL_RUNTIME_COMMON_H
#define ASDL_RUNTIME_COMMON_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef ASDL_MALLOC
#define ASDL_MALLOC(n) malloc(n)
#endif
#ifndef ASDL_FREE
#define ASDL_FREE(p) free(p)
#endif
static inline char *ASDL_copy_text(const char *text, size_t length) {
    if ((!text && length) || length == SIZE_MAX) return NULL;
    char *copy = (char *)ASDL_MALLOC(length + 1);
    if (copy) {
        if (length) memcpy(copy, text, length);
        copy[length] = 0;
    }
    return copy;
}
#endif
)ASDL";

inline constexpr std::string_view list = R"ASDL(#ifndef ASDL_VECTOR_H
#define ASDL_VECTOR_H
struct ASDLVector { void *data; size_t length, capacity, element_size; };
static inline void ASDLVector_init(struct ASDLVector *v, size_t element_size) {
    if (v) *v = (struct ASDLVector){NULL, 0, 0, element_size};
}
static inline void ASDLVector_destroy(struct ASDLVector *v) {
    if (v) { ASDL_FREE(v->data); ASDLVector_init(v, v->element_size); }
}
static inline bool ASDLVector_reserve(struct ASDLVector *v, size_t capacity) {
    if (!v || !v->element_size || capacity > SIZE_MAX / v->element_size) return false;
    if (capacity <= v->capacity) return true;
    void *data = ASDL_MALLOC(capacity * v->element_size);
    if (!data) return false;
    if (v->length) memcpy(data, v->data, v->length * v->element_size);
    ASDL_FREE(v->data);
    v->data = data;
    v->capacity = capacity;
    return true;
}
static inline void *ASDLVector_at(const struct ASDLVector *v, size_t index) {
    return v && index < v->length ? (unsigned char *)v->data + index * v->element_size : NULL;
}
static inline bool ASDLVector_set(struct ASDLVector *v, size_t index, const void *value) {
    void *slot = ASDLVector_at(v, index);
    if (!slot || !value) return false;
    memmove(slot, value, v->element_size);
    return true;
}
static inline bool ASDLVector_resize(struct ASDLVector *v, size_t length) {
    if (!ASDLVector_reserve(v, length)) return false;
    if (length > v->length) memset((unsigned char *)v->data + v->length * v->element_size,
        0, (length - v->length) * v->element_size);
    v->length = length;
    return true;
}
static inline bool ASDLVector_insert(struct ASDLVector *v, size_t index, const void *value) {
    if (!v || !value || !v->element_size || index > v->length || v->length == SIZE_MAX) return false;
    /* Snapshot first: value may point into storage moved by reserve/memmove. */
    void *copy = ASDL_MALLOC(v->element_size);
    if (!copy) return false;
    memcpy(copy, value, v->element_size);
    size_t capacity = v->capacity;
    if (capacity <= v->length) capacity = capacity && capacity <= SIZE_MAX / 2 ? capacity * 2 : v->length + 1;
    if (!ASDLVector_reserve(v, capacity)) { ASDL_FREE(copy); return false; }
    unsigned char *slot = (unsigned char *)v->data + index * v->element_size;
    memmove(slot + v->element_size, slot, (v->length - index) * v->element_size);
    memcpy(slot, copy, v->element_size);
    ASDL_FREE(copy);
    ++v->length;
    return true;
}
static inline bool ASDLVector_push(struct ASDLVector *v, const void *value) {
    return v && ASDLVector_insert(v, v->length, value);
}
static inline bool ASDLVector_erase(struct ASDLVector *v, size_t index, size_t count) {
    if (!v || index > v->length || count > v->length - index) return false;
    if (count) {
        unsigned char *slot = (unsigned char *)v->data + index * v->element_size;
        memmove(slot, slot + count * v->element_size, (v->length - index - count) * v->element_size);
        v->length -= count;
    }
    return true;
}
static inline bool ASDLVector_pop(struct ASDLVector *v, void *value) {
    if (!v || !v->length) return false;
    if (value) memmove(value, ASDLVector_at(v, v->length - 1), v->element_size);
    --v->length;
    return true;
}
static inline void ASDLVector_clear(struct ASDLVector *v) { if (v) v->length = 0; }
static inline void ASDLVector_sort(struct ASDLVector *v, int (*compare)(const void *, const void *)) {
    if (v && v->length > 1 && compare) qsort(v->data, v->length, v->element_size, compare);
}
static inline void ASDLVector_reverse(struct ASDLVector *v) {
    if (!v) return;
    for (size_t i = 0; i < v->length / 2; ++i) {
        unsigned char *a = (unsigned char *)ASDLVector_at(v, i);
        unsigned char *b = (unsigned char *)ASDLVector_at(v, v->length - i - 1);
        for (size_t j = 0; j < v->element_size; ++j) { unsigned char c = a[j]; a[j] = b[j]; b[j] = c; }
    }
}
#endif
)ASDL";

inline constexpr std::string_view array = R"ASDL(#ifndef ASDL_FIXED_ARRAY_H
#define ASDL_FIXED_ARRAY_H
struct ASDLFixedArray { void *data; size_t length, element_size; };
static inline bool ASDLFixedArray_init(struct ASDLFixedArray *a, size_t element_size, size_t length) {
    if (!a) return false;
    *a = (struct ASDLFixedArray){NULL, 0, element_size};
    if (!element_size || !length || length > SIZE_MAX / element_size) return false;
    a->data = ASDL_MALLOC(length * element_size);
    if (!a->data) return false;
    memset(a->data, 0, length * element_size);
    a->length = length;
    return true;
}
static inline void ASDLFixedArray_destroy(struct ASDLFixedArray *a) {
    if (a) { ASDL_FREE(a->data); *a = (struct ASDLFixedArray){0}; }
}
static inline void *ASDLFixedArray_at(const struct ASDLFixedArray *a, size_t index) {
    return a && index < a->length ? (unsigned char *)a->data + index * a->element_size : NULL;
}
static inline bool ASDLFixedArray_set(struct ASDLFixedArray *a, size_t index, const void *value) {
    void *slot = ASDLFixedArray_at(a, index);
    if (!slot || !value) return false;
    memmove(slot, value, a->element_size);
    return true;
}
static inline bool ASDLFixedArray_fill(struct ASDLFixedArray *a, const void *value) {
    if (!a || !a->data || !value) return false;
    /* First slot snapshots a possibly aliased element. */
    memmove(a->data, value, a->element_size);
    for (size_t i = 1; i < a->length; ++i) memcpy(ASDLFixedArray_at(a, i), a->data, a->element_size);
    return true;
}
static inline bool ASDLFixedArray_copy(struct ASDLFixedArray *to, const struct ASDLFixedArray *from) {
    if (!to || !from || to->length != from->length || to->element_size != from->element_size) return false;
    if (to->length) memmove(to->data, from->data, to->length * to->element_size);
    return true;
}
static inline void ASDLFixedArray_sort(struct ASDLFixedArray *a, int (*compare)(const void *, const void *)) {
    if (a && a->length > 1 && compare) qsort(a->data, a->length, a->element_size, compare);
}
#endif
)ASDL";

inline constexpr std::string_view map = R"ASDL(#ifndef ASDL_HASH_MAP_H
#define ASDL_HASH_MAP_H
struct ASDLHashEntry { void *key, *value; size_t hash; unsigned char state; };
struct ASDLHashMap {
    struct ASDLHashEntry *entries;
    size_t length, capacity, key_size, value_size;
    size_t (*hash)(const void *);
    int (*equal)(const void *, const void *);
};
static inline void ASDLHashMap_init(struct ASDLHashMap *m, size_t key_size, size_t value_size,
        size_t (*hash)(const void *), int (*equal)(const void *, const void *)) {
    if (m) *m = (struct ASDLHashMap){NULL, 0, 0, key_size, value_size, hash, equal};
}
static inline void ASDLHashMap_clear(struct ASDLHashMap *m) {
    if (!m) return;
    for (size_t i = 0; i < m->capacity; ++i) {
        if (m->entries[i].state == 1) { ASDL_FREE(m->entries[i].key); ASDL_FREE(m->entries[i].value); }
        m->entries[i] = (struct ASDLHashEntry){0};
    }
    m->length = 0;
}
static inline void ASDLHashMap_destroy(struct ASDLHashMap *m) {
    if (!m) return;
    ASDLHashMap_clear(m);
    ASDL_FREE(m->entries);
    m->entries = NULL;
    m->capacity = 0;
}
static inline bool ASDLHashMap_reserve(struct ASDLHashMap *m, size_t capacity) {
    if (!m || !m->key_size || !m->value_size || !m->hash) return false;
    if (capacity <= m->capacity) return true;
    if (capacity > SIZE_MAX / sizeof(*m->entries)) return false;
    struct ASDLHashEntry *entries = (struct ASDLHashEntry *)ASDL_MALLOC(capacity * sizeof(*entries));
    if (!entries) return false;
    for (size_t i = 0; i < capacity; ++i) entries[i] = (struct ASDLHashEntry){0};
    for (size_t i = 0; i < m->capacity; ++i) if (m->entries[i].state == 1) {
        size_t j = m->entries[i].hash % capacity;
        while (entries[j].state) j = j + 1 == capacity ? 0 : j + 1;
        entries[j] = m->entries[i];
    }
    ASDL_FREE(m->entries);
    m->entries = entries;
    m->capacity = capacity;
    return true;
}
static inline size_t ASDLHashMap_find(const struct ASDLHashMap *m, const void *key, size_t hash) {
    if (!m || !m->capacity || !key) return SIZE_MAX;
    size_t index = hash % m->capacity;
    for (size_t n = 0; n < m->capacity; ++n) {
        const struct ASDLHashEntry *e = &m->entries[index];
        if (!e->state) return SIZE_MAX;
        if (e->state == 1 && e->hash == hash && (m->equal ? m->equal(e->key, key)
                : memcmp(e->key, key, m->key_size) == 0)) return index;
        index = index + 1 == m->capacity ? 0 : index + 1;
    }
    return SIZE_MAX;
}
static inline void *ASDLHashMap_get(const struct ASDLHashMap *m, const void *key) {
    if (!m || !m->hash || !key) return NULL;
    size_t index = ASDLHashMap_find(m, key, m->hash(key));
    return index == SIZE_MAX ? NULL : m->entries[index].value;
}
static inline bool ASDLHashMap_contains(const struct ASDLHashMap *m, const void *key) {
    return ASDLHashMap_get(m, key) != NULL;
}
static inline bool ASDLHashMap_set(struct ASDLHashMap *m, const void *key, const void *value) {
    if (!m || !m->hash || !key || !value || !m->key_size || !m->value_size) return false;
    size_t hash = m->hash(key), index = ASDLHashMap_find(m, key, hash);
    if (index != SIZE_MAX) { memmove(m->entries[index].value, value, m->value_size); return true; }
    void *k = ASDL_MALLOC(m->key_size), *v = ASDL_MALLOC(m->value_size);
    if (!k || !v) { ASDL_FREE(k); ASDL_FREE(v); return false; }
    memcpy(k, key, m->key_size);
    memcpy(v, value, m->value_size);
    if (m->length >= m->capacity / 2) {
        size_t capacity = m->capacity ? m->capacity * 2 : 16;
        if (capacity < m->capacity || !ASDLHashMap_reserve(m, capacity)) {
            ASDL_FREE(k); ASDL_FREE(v); return false;
        }
    }
    index = hash % m->capacity;
    while (m->entries[index].state == 1) index = index + 1 == m->capacity ? 0 : index + 1;
    m->entries[index] = (struct ASDLHashEntry){k, v, hash, 1};
    ++m->length;
    return true;
}
static inline bool ASDLHashMap_remove(struct ASDLHashMap *m, const void *key) {
    if (!m || !m->hash || !key) return false;
    size_t index = ASDLHashMap_find(m, key, m->hash(key));
    if (index == SIZE_MAX) return false;
    ASDL_FREE(m->entries[index].key);
    ASDL_FREE(m->entries[index].value);
    m->entries[index] = (struct ASDLHashEntry){NULL, NULL, 0, 2};
    --m->length;
    return true;
}
/* Iteration: initialize cursor to zero; returned pointers borrow map storage. */
static inline bool ASDLHashMap_next(const struct ASDLHashMap *m, size_t *cursor, const void **key, void **value) {
    if (!m || !cursor) return false;
    while (*cursor < m->capacity) {
        const struct ASDLHashEntry *e = &m->entries[(*cursor)++];
        if (e->state == 1) { if (key) *key = e->key; if (value) *value = e->value; return true; }
    }
    return false;
}
#endif
)ASDL";

inline constexpr std::string_view buffer = R"ASDL(#ifndef ASDL_BYTE_BUFFER_H
#define ASDL_BYTE_BUFFER_H
struct ASDLByteBuffer { unsigned char *data; size_t length, capacity, position; };
static inline void ASDLByteBuffer_init(struct ASDLByteBuffer *b) { if (b) *b = (struct ASDLByteBuffer){0}; }
static inline void ASDLByteBuffer_destroy(struct ASDLByteBuffer *b) {
    if (b) { ASDL_FREE(b->data); ASDLByteBuffer_init(b); }
}
static inline bool ASDLByteBuffer_reserve(struct ASDLByteBuffer *b, size_t capacity) {
    if (!b) return false;
    if (capacity <= b->capacity) return true;
    unsigned char *data = (unsigned char *)ASDL_MALLOC(capacity);
    if (!data) return false;
    if (b->length) memcpy(data, b->data, b->length);
    ASDL_FREE(b->data);
    b->data = data; b->capacity = capacity;
    return true;
}
static inline bool ASDLByteBuffer_append(struct ASDLByteBuffer *b, const void *data, size_t length) {
    if (!b || (length && !data) || length > SIZE_MAX - b->length) return false;
    if (!length) return true;
    /* Snapshot permits self-append without comparing unrelated pointers. */
    unsigned char *copy = (unsigned char *)ASDL_MALLOC(length);
    if (!copy) return false;
    memcpy(copy, data, length);
    size_t need = b->length + length, capacity = b->capacity;
    if (capacity < need) capacity = capacity <= SIZE_MAX / 2 && capacity * 2 > need ? capacity * 2 : need;
    if (!ASDLByteBuffer_reserve(b, capacity)) { ASDL_FREE(copy); return false; }
    memcpy(b->data + b->length, copy, length);
    ASDL_FREE(copy);
    b->length = need;
    return true;
}
static inline bool ASDLByteBuffer_put(struct ASDLByteBuffer *b, unsigned char byte) {
    return ASDLByteBuffer_append(b, &byte, 1);
}
static inline size_t ASDLByteBuffer_read(struct ASDLByteBuffer *b, void *data, size_t length) {
    if (!b || !data || b->position > b->length) return 0;
    if (length > b->length - b->position) length = b->length - b->position;
    if (length) memmove(data, b->data + b->position, length);
    b->position += length;
    return length;
}
static inline bool ASDLByteBuffer_seek(struct ASDLByteBuffer *b, size_t position) {
    if (!b || position > b->length) return false;
    b->position = position;
    return true;
}
static inline bool ASDLByteBuffer_truncate(struct ASDLByteBuffer *b, size_t length) {
    if (!b || length > b->length) return false;
    b->length = length;
    if (b->position > length) b->position = length;
    return true;
}
static inline void ASDLByteBuffer_clear(struct ASDLByteBuffer *b) { if (b) b->length = b->position = 0; }
static inline const char *ASDLByteBuffer_cstr(struct ASDLByteBuffer *b) {
    if (!b || b->length == SIZE_MAX || !ASDLByteBuffer_reserve(b, b->length + 1)) return NULL;
    b->data[b->length] = 0;
    return (const char *)b->data;
}
#endif
)ASDL";

inline constexpr std::string_view port = R"ASDL(#ifndef ASDL_PORT_H
#define ASDL_PORT_H
struct ASDLPort {
    FILE *file;
    const unsigned char *input;
    size_t length, position;
    struct ASDLByteBuffer output;
    int kind, error;
    bool owned;
};
static inline void ASDLPort_init(struct ASDLPort *p) { if (p) *p = (struct ASDLPort){0}; }
static inline bool ASDLPort_open(struct ASDLPort *p, const char *path, const char *mode) {
    if (!p || !path || !mode) return false;
    ASDLPort_init(p);
    p->file = fopen(path, mode);
    p->kind = 1; p->owned = true;
    if (!p->file) p->error = errno;
    return p->file != NULL;
}
static inline void ASDLPort_file(struct ASDLPort *p, FILE *file, bool owned) {
    if (p) { ASDLPort_init(p); p->kind = 1; p->file = file; p->owned = owned; }
}
static inline bool ASDLPort_input(struct ASDLPort *p, const void *data, size_t length) {
    if (!p || (length && !data)) return false;
    ASDLPort_init(p); p->kind = 2; p->input = (const unsigned char *)data; p->length = length;
    return true;
}
static inline void ASDLPort_output(struct ASDLPort *p) { if (p) { ASDLPort_init(p); p->kind = 3; } }
static inline int ASDLPort_close(struct ASDLPort *p) {
    if (!p) return EOF;
    int status = p->file && p->owned ? fclose(p->file) : 0;
    ASDLByteBuffer_destroy(&p->output); ASDLPort_init(p);
    return status;
}
static inline size_t ASDLPort_read(struct ASDLPort *p, void *data, size_t length) {
    if (!p || !data) return 0;
    if (p->kind == 1 && p->file) {
        size_t count = fread(data, 1, length, p->file);
        if (ferror(p->file)) p->error = EIO;
        return count;
    }
    if (p->kind != 2) { p->error = EBADF; return 0; }
    if (length > p->length - p->position) length = p->length - p->position;
    if (length) memcpy(data, p->input + p->position, length);
    p->position += length;
    return length;
}
static inline size_t ASDLPort_write(struct ASDLPort *p, const void *data, size_t length) {
    if (!p || (length && !data)) return 0;
    if (!length) return 0;
    if (p->kind == 1 && p->file) {
        size_t count = fwrite(data, 1, length, p->file);
        if (count != length) p->error = EIO;
        return count;
    }
    if (p->kind != 3) { p->error = EBADF; return 0; }
    if (!ASDLByteBuffer_append(&p->output, data, length)) { p->error = ENOMEM; return 0; }
    return length;
}
static inline int ASDLPort_get(struct ASDLPort *p) { unsigned char c; return ASDLPort_read(p, &c, 1) ? c : EOF; }
static inline bool ASDLPort_put(struct ASDLPort *p, unsigned char c) { return ASDLPort_write(p, &c, 1) == 1; }
static inline bool ASDLPort_flush(struct ASDLPort *p) {
    if (!p || !p->kind) return false;
    if (p->kind != 1) return true;
    if (!p->file || fflush(p->file)) { p->error = EIO; return false; }
    return true;
}
static inline bool ASDLPort_seek(struct ASDLPort *p, long offset, int whence) {
    if (!p) return false;
    if (p->kind == 1 && p->file) return fseek(p->file, offset, whence) == 0;
    if (p->kind != 2) return false;
    size_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? p->position : p->length;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return false;
    if (offset < 0) {
        uintmax_t magnitude = (uintmax_t)(-(offset + 1)) + 1;
        if (magnitude > base) return false;
        p->position = base - (size_t)magnitude;
    } else {
        if ((uintmax_t)offset > p->length - base) return false;
        p->position = base + (size_t)offset;
    }
    return true;
}
static inline bool ASDLPort_eof(const struct ASDLPort *p) {
    return !p || (p->kind == 1 ? !p->file || feof(p->file) : p->kind == 2 && p->position == p->length);
}
static inline bool ASDLPort_read_line(struct ASDLPort *p, struct ASDLByteBuffer *line) {
    if (!line) return false;
    ASDLByteBuffer_clear(line);
    int c;
    while ((c = ASDLPort_get(p)) != EOF) {
        if (!ASDLByteBuffer_put(line, (unsigned char)c)) return false;
        if (c == '\n') break;
    }
    return line->length != 0;
}
#endif
)ASDL";

inline constexpr std::string_view path = R"ASDL(#ifndef ASDL_PATH_H
#define ASDL_PATH_H
struct ASDLPath { char *text; };
static inline bool ASDLPath_init(struct ASDLPath *p, const char *text) {
    if (!p) return false;
    p->text = text ? ASDL_copy_text(text, strlen(text)) : NULL;
    return p->text != NULL;
}
static inline void ASDLPath_destroy(struct ASDLPath *p) { if (p) { ASDL_FREE(p->text); p->text = NULL; } }
static inline bool ASDLPath_is_absolute(const struct ASDLPath *p) { return p && p->text && p->text[0] == '/'; }
static inline bool ASDLPath_join(struct ASDLPath *out, const struct ASDLPath *base, const char *part) {
    if (!out || !base || !base->text || !part) return false;
    if (*part == '/') { char *s = ASDL_copy_text(part, strlen(part)); if (!s) return false; ASDLPath_destroy(out); out->text = s; return true; }
    size_t a = strlen(base->text), b = strlen(part);
    if (a > SIZE_MAX - 2 || b > SIZE_MAX - a - 2) return false;
    char *s = (char *)ASDL_MALLOC(a + b + 2);
    if (!s) return false;
    memcpy(s, base->text, a);
    if (a && s[a - 1] != '/') s[a++] = '/';
    memcpy(s + a, part, b + 1);
    ASDLPath_destroy(out); out->text = s;
    return true;
}
static inline const char *ASDLPath_name(const struct ASDLPath *p) {
    if (!p || !p->text) return NULL;
    const char *slash = strrchr(p->text, '/');
    return slash ? slash + 1 : p->text;
}
static inline const char *ASDLPath_suffix(const struct ASDLPath *p) {
    const char *name = ASDLPath_name(p), *dot = name ? strrchr(name, '.') : NULL;
    return dot && dot != name ? dot : "";
}
static inline bool ASDLPath_parent(struct ASDLPath *out, const struct ASDLPath *p) {
    if (!out || !p || !p->text) return false;
    size_t n = strlen(p->text);
    while (n > 1 && p->text[n - 1] == '/') --n;
    while (n && p->text[n - 1] != '/') --n;
    while (n > 1 && p->text[n - 1] == '/') --n;
    char *s = n ? ASDL_copy_text(p->text, n) : ASDL_copy_text(".", 1);
    if (!s) return false;
    ASDLPath_destroy(out); out->text = s;
    return true;
}
static inline bool ASDLPath_exists(const struct ASDLPath *p) { struct stat st; return p && p->text && stat(p->text, &st) == 0; }
static inline bool ASDLPath_is_file(const struct ASDLPath *p) { struct stat st; return p && p->text && stat(p->text, &st) == 0 && S_ISREG(st.st_mode); }
static inline bool ASDLPath_is_dir(const struct ASDLPath *p) { struct stat st; return p && p->text && stat(p->text, &st) == 0 && S_ISDIR(st.st_mode); }
static inline bool ASDLPath_mkdir(const struct ASDLPath *p, mode_t mode) { return p && p->text && mkdir(p->text, mode) == 0; }
static inline bool ASDLPath_remove(const struct ASDLPath *p) { return p && p->text && remove(p->text) == 0; }
static inline bool ASDLPath_rename(const struct ASDLPath *p, const struct ASDLPath *to) { return p && p->text && to && to->text && rename(p->text, to->text) == 0; }
static inline bool ASDLPath_read(const struct ASDLPath *p, struct ASDLByteBuffer *out) {
    if (!p || !p->text || !out) return false;
    FILE *f = fopen(p->text, "rb");
    if (!f) return false;
    struct ASDLByteBuffer result = {0};
    unsigned char bytes[4096]; size_t n; bool ok = true;
    while ((n = fread(bytes, 1, sizeof(bytes), f)) != 0) if (!ASDLByteBuffer_append(&result, bytes, n)) { ok = false; break; }
    if (ferror(f)) ok = false;
    if (fclose(f)) ok = false;
    if (ok) { ASDLByteBuffer_destroy(out); *out = result; } else ASDLByteBuffer_destroy(&result);
    return ok;
}
static inline bool ASDLPath_write(const struct ASDLPath *p, const void *data, size_t length) {
    if (!p || !p->text || (length && !data)) return false;
    FILE *f = fopen(p->text, "wb");
    if (!f) return false;
    bool ok = !length || fwrite(data, 1, length, f) == length;
    if (fclose(f)) ok = false;
    return ok;
}
#endif
)ASDL";

inline constexpr std::string_view event = R"ASDL(#ifndef ASDL_EVENT_HANDLER_H
#define ASDL_EVENT_HANDLER_H
enum ASDLEventKind { ASDL_EVENT_DATA, ASDL_EVENT_EOF, ASDL_EVENT_RESET,
    ASDL_EVENT_OPEN, ASDL_EVENT_CLOSE, ASDL_EVENT_ATOM, ASDL_EVENT_STRING, ASDL_EVENT_QUOTE };
enum ASDLEventResult { ASDL_EVENT_ERROR = -1, ASDL_EVENT_CONTINUE = 0, ASDL_EVENT_PAUSE = 1 };
struct ASDLEvent { enum ASDLEventKind kind; const unsigned char *data; size_t length, offset, depth; };
struct ASDLEventHandler { int (*callback)(void *, const struct ASDLEvent *); void *context; };
static inline int ASDLEventHandler_call(const struct ASDLEventHandler *handler, const struct ASDLEvent *event) {
    if (!handler || !handler->callback) return ASDL_EVENT_CONTINUE;
    int result = handler->callback(handler->context, event);
    return result == ASDL_EVENT_CONTINUE || result == ASDL_EVENT_PAUSE ? result : ASDL_EVENT_ERROR;
}
#endif
)ASDL";

inline constexpr std::string_view sexpr = R"ASDL(#ifndef ASDL_SEXPR_H
#define ASDL_SEXPR_H
struct ASDLSexpr {
    struct ASDLByteBuffer token;
    struct ASDLEventHandler handler;
    size_t offset, start, depth;
    int state; /* 0 layout, 1 atom, 2 string, 3 escape, 4 comment */
    bool failed, finished, quoted;
};
static inline void ASDLSexpr_init(struct ASDLSexpr *s, struct ASDLEventHandler handler) {
    if (s) { *s = (struct ASDLSexpr){0}; s->handler = handler; }
}
static inline void ASDLSexpr_destroy(struct ASDLSexpr *s) {
    if (s) { ASDLByteBuffer_destroy(&s->token); *s = (struct ASDLSexpr){0}; }
}
static inline void ASDLSexpr_reset(struct ASDLSexpr *s) {
    if (!s) return;
    struct ASDLEventHandler handler = s->handler;
    ASDLSexpr_destroy(s); ASDLSexpr_init(s, handler);
}
static inline int ASDLSexpr_emit(struct ASDLSexpr *s, enum ASDLEventKind kind) {
    struct ASDLEvent event = {kind, s->token.data, s->token.length, s->start, s->depth};
    int result = ASDLEventHandler_call(&s->handler, &event);
    ASDLByteBuffer_clear(&s->token);
    if (result < 0) s->failed = true;
    return result;
}
/* A PAUSE saves the continuation in s. Resume with the unconsumed suffix.
 * Token bytes are borrowed only during the callback. final may be repeated
 * with an empty suffix after PAUSE to finish the EOF continuation. */
static inline int ASDLSexpr_feed(struct ASDLSexpr *s, const void *data, size_t length, bool final, size_t *consumed) {
    size_t i = 0;
    const unsigned char *bytes = (const unsigned char *)data;
    if (consumed) *consumed = 0;
    if (!s || s->failed || (length && !data) || (s->finished && length)) return ASDL_EVENT_ERROR;
    if (s->finished) return ASDL_EVENT_CONTINUE;
    while (i < length) {
        unsigned char c = bytes[i];
        int result = ASDL_EVENT_CONTINUE;
        if (s->state == 1 && (isspace(c) || c == '(' || c == ')' || c == ';' || c == '"' || c == '\'')) {
            s->state = 0;
            result = ASDLSexpr_emit(s, ASDL_EVENT_ATOM);
            if (result) { if (consumed) *consumed = i; return result; }
            continue;
        }
        if (s->offset == SIZE_MAX) { s->failed = true; break; }
        ++i; ++s->offset;
        if (s->state == 4) { if (c == '\n') s->state = 0; }
        else if (s->state == 3) {
            if (c == 'n') c = '\n'; else if (c == 'r') c = '\r'; else if (c == 't') c = '\t';
            else if (c != '\\' && c != '"') { s->failed = true; break; }
            s->state = 2;
            if (!ASDLByteBuffer_put(&s->token, c)) { s->failed = true; break; }
        } else if (s->state == 2) {
            if (c == '\\') s->state = 3;
            else if (c == '"') { s->state = 0; result = ASDLSexpr_emit(s, ASDL_EVENT_STRING); }
            else if (!ASDLByteBuffer_put(&s->token, c)) { s->failed = true; break; }
        } else if (s->state == 1) {
            if (!c || !ASDLByteBuffer_put(&s->token, c)) { s->failed = true; break; }
        } else if (!isspace(c)) {
            s->start = s->offset - 1;
            if (c == ';') s->state = 4;
            else if (c == '"') { s->state = 2; s->quoted = false; }
            else if (c == '(') {
                if (s->depth == SIZE_MAX) { s->failed = true; break; }
                s->quoted = false; ++s->depth; result = ASDLSexpr_emit(s, ASDL_EVENT_OPEN);
            } else if (c == ')') {
                if (!s->depth || s->quoted) { s->failed = true; break; }
                --s->depth; result = ASDLSexpr_emit(s, ASDL_EVENT_CLOSE);
            } else if (c == '\'') { s->quoted = true; result = ASDLSexpr_emit(s, ASDL_EVENT_QUOTE); }
            else {
                s->state = 1; s->quoted = false;
                if (!c || !ASDLByteBuffer_put(&s->token, c)) { s->failed = true; break; }
            }
        }
        if (result) { if (consumed) *consumed = i; return result; }
    }
    if (consumed) *consumed = i;
    if (s->failed) return ASDL_EVENT_ERROR;
    if (final) {
        if (s->state == 1) {
            s->state = 0;
            int result = ASDLSexpr_emit(s, ASDL_EVENT_ATOM);
            if (result) return result;
        }
        if (s->state == 2 || s->state == 3 || s->depth || s->quoted) { s->failed = true; return ASDL_EVENT_ERROR; }
        s->finished = true; s->start = s->offset;
        return ASDLSexpr_emit(s, ASDL_EVENT_EOF);
    }
    return ASDL_EVENT_CONTINUE;
}
/* Writer is also continuation-based: deliver each event to this callback.
 * Output is canonical whitespace-separated S-expression text, not an AST
 * serialization format. Strings preserve arbitrary bytes, including NUL. */
static inline int ASDLSexpr_write(void *context, const struct ASDLEvent *event) {
    struct ASDLPort *port = (struct ASDLPort *)context;
    if (!port || !event || (event->length && !event->data)) return ASDL_EVENT_ERROR;
    if (event->kind == ASDL_EVENT_EOF) return ASDLPort_flush(port) ? 0 : -1;
    if (event->kind == ASDL_EVENT_OPEN) return ASDLPort_put(port, '(') ? 0 : -1;
    if (event->kind == ASDL_EVENT_CLOSE) return ASDLPort_write(port, ") ", 2) == 2 ? 0 : -1;
    if (event->kind == ASDL_EVENT_QUOTE) return ASDLPort_put(port, '\'') ? 0 : -1;
    if (event->kind != ASDL_EVENT_ATOM && event->kind != ASDL_EVENT_STRING) return -1;
    if (event->kind == ASDL_EVENT_ATOM) {
        if (!event->length) return -1;
        for (size_t i = 0; i < event->length; ++i) {
            unsigned char c = event->data[i];
            if (!c || isspace(c) || strchr("();\"'", c)) return -1;
        }
        if (ASDLPort_write(port, event->data, event->length) != event->length) return -1;
    } else {
        if (!ASDLPort_put(port, '"')) return -1;
        for (size_t i = 0; i < event->length; ++i) {
            unsigned char c = event->data[i];
            if (c == '\n' || c == '\r' || c == '\t' || c == '\\' || c == '"') {
                if (!ASDLPort_put(port, '\\')) return -1;
                if (c == '\n') c = 'n'; else if (c == '\r') c = 'r'; else if (c == '\t') c = 't';
            }
            if (!ASDLPort_put(port, c)) return -1;
        }
        if (!ASDLPort_put(port, '"')) return -1;
    }
    return ASDLPort_put(port, ' ') ? 0 : -1;
}
#endif
)ASDL";

inline constexpr std::string_view regexp = R"ASDL(#ifndef ASDL_REGEXP_H
#define ASDL_REGEXP_H
#include <regex.h>
struct ASDLRegexp { regex_t *regex; bool compiled; int error; };
static inline void ASDLRegexp_init(struct ASDLRegexp *r) { if (r) *r = (struct ASDLRegexp){0}; }
static inline void ASDLRegexp_destroy(struct ASDLRegexp *r) {
    if (r) { if (r->compiled) regfree(r->regex); ASDL_FREE(r->regex); ASDLRegexp_init(r); }
}
static inline bool ASDLRegexp_compile(struct ASDLRegexp *r, const char *pattern, int flags) {
    if (!r || !pattern) return false;
    ASDLRegexp_destroy(r);
    r->regex = (regex_t *)ASDL_MALLOC(sizeof(*r->regex));
    if (!r->regex) { r->error = REG_ESPACE; return false; }
    r->error = regcomp(r->regex, pattern, flags);
    r->compiled = r->error == 0;
    if (!r->compiled) { ASDL_FREE(r->regex); r->regex = NULL; }
    return r->compiled;
}
static inline int ASDLRegexp_exec(struct ASDLRegexp *r, const char *text, size_t count, regmatch_t *matches, int flags) {
    if (!r || !r->compiled || !text || (count && !matches)) return REG_BADPAT;
    r->error = regexec(r->regex, text, count, matches, flags);
    return r->error;
}
static inline bool ASDLRegexp_test(struct ASDLRegexp *r, const char *text) { return ASDLRegexp_exec(r, text, 0, NULL, 0) == 0; }
static inline size_t ASDLRegexp_error(const struct ASDLRegexp *r, char *buffer, size_t size) {
    return regerror(r ? r->error : REG_BADPAT, r && r->compiled ? r->regex : NULL, buffer, size);
}
/* Literal replacement of the first match; captures are exposed by exec. */
static inline bool ASDLRegexp_replace(struct ASDLRegexp *r, const char *text, const char *replacement, struct ASDLByteBuffer *out) {
    if (!r || !text || !replacement || !out) return false;
    regmatch_t match = {-1, -1};
    if (ASDLRegexp_exec(r, text, 1, &match, 0) || match.rm_so < 0 || match.rm_eo < match.rm_so) return false;
    struct ASDLByteBuffer result = {0};
    bool ok = ASDLByteBuffer_append(&result, text, (size_t)match.rm_so)
        && ASDLByteBuffer_append(&result, replacement, strlen(replacement))
        && ASDLByteBuffer_append(&result, text + match.rm_eo, strlen(text + match.rm_eo));
    if (ok) { ASDLByteBuffer_destroy(out); *out = result; } else ASDLByteBuffer_destroy(&result);
    return ok;
}
#endif
)ASDL";

inline constexpr std::string_view parsec = R"ASDL(#ifndef ASDL_PARSEC_H
#define ASDL_PARSEC_H
enum ASDLParsecKind { ASDL_PARSEC_LITERAL, ASDL_PARSEC_RANGE, ASDL_PARSEC_ANY,
    ASDL_PARSEC_END, ASDL_PARSEC_SEQUENCE, ASDL_PARSEC_CHOICE, ASDL_PARSEC_REPEAT, ASDL_PARSEC_REF };
struct ASDLParsecNode {
    enum ASDLParsecKind kind;
    struct ASDLParsecNode *left, *right, *next;
    char *text;
    size_t length, minimum, maximum;
    unsigned char low, high;
};
struct ASDLParsecRule { char *name; struct ASDLParsecNode *node; struct ASDLParsecRule *next; };
struct ASDLParsec {
    struct ASDLParsecNode *nodes, *root;
    struct ASDLParsecRule *rules;
    size_t max_depth, max_steps, error_offset;
    const char *error;
};
static inline void ASDLParsec_init(struct ASDLParsec *p) {
    if (p) { *p = (struct ASDLParsec){0}; p->max_depth = 256; p->max_steps = 1000000; }
}
static inline void ASDLParsec_destroy(struct ASDLParsec *p) {
    if (!p) return;
    while (p->nodes) { struct ASDLParsecNode *n = p->nodes; p->nodes = n->next; ASDL_FREE(n->text); ASDL_FREE(n); }
    while (p->rules) { struct ASDLParsecRule *r = p->rules; p->rules = r->next; ASDL_FREE(r->name); ASDL_FREE(r); }
    ASDLParsec_init(p);
}
static inline struct ASDLParsecNode *ASDLParsec_node(struct ASDLParsec *p, enum ASDLParsecKind kind) {
    if (!p || p->error) return NULL;
    struct ASDLParsecNode *n = (struct ASDLParsecNode *)ASDL_MALLOC(sizeof(*n));
    if (!n) { p->error = "out of memory"; return NULL; }
    *n = (struct ASDLParsecNode){0}; n->kind = kind; n->next = p->nodes; p->nodes = n;
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_literal(struct ASDLParsec *p, const void *text, size_t length) {
    if (length && !text) { if (p) p->error = "invalid literal"; return NULL; }
    struct ASDLParsecNode *n = ASDLParsec_node(p, ASDL_PARSEC_LITERAL);
    if (n) { n->text = ASDL_copy_text((const char *)text, length); n->length = length; if (!n->text) { p->error = "out of memory"; return NULL; } }
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_range(struct ASDLParsec *p, unsigned char low, unsigned char high) {
    if (low > high) { if (p) p->error = "invalid range"; return NULL; }
    struct ASDLParsecNode *n = ASDLParsec_node(p, ASDL_PARSEC_RANGE);
    if (n) { n->low = low; n->high = high; }
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_any(struct ASDLParsec *p) { return ASDLParsec_node(p, ASDL_PARSEC_ANY); }
static inline struct ASDLParsecNode *ASDLParsec_end(struct ASDLParsec *p) { return ASDLParsec_node(p, ASDL_PARSEC_END); }
static inline struct ASDLParsecNode *ASDLParsec_pair(struct ASDLParsec *p, enum ASDLParsecKind kind, struct ASDLParsecNode *a, struct ASDLParsecNode *b) {
    if (!a || !b) { if (p) p->error = "missing operand"; return NULL; }
    struct ASDLParsecNode *n = ASDLParsec_node(p, kind);
    if (n) { n->left = a; n->right = b; }
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_sequence(struct ASDLParsec *p, struct ASDLParsecNode *a, struct ASDLParsecNode *b) {
    return ASDLParsec_pair(p, ASDL_PARSEC_SEQUENCE, a, b);
}
static inline struct ASDLParsecNode *ASDLParsec_choice(struct ASDLParsec *p, struct ASDLParsecNode *a, struct ASDLParsecNode *b) {
    return ASDLParsec_pair(p, ASDL_PARSEC_CHOICE, a, b);
}
static inline struct ASDLParsecNode *ASDLParsec_repeat(struct ASDLParsec *p, struct ASDLParsecNode *child, size_t minimum, size_t maximum) {
    if (!child || minimum > maximum) { if (p) p->error = "invalid repetition"; return NULL; }
    struct ASDLParsecNode *n = ASDLParsec_node(p, ASDL_PARSEC_REPEAT);
    if (n) { n->left = child; n->minimum = minimum; n->maximum = maximum; }
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_ref(struct ASDLParsec *p, const char *name) {
    if (!name) return NULL;
    struct ASDLParsecNode *n = ASDLParsec_node(p, ASDL_PARSEC_REF);
    if (n) { n->text = ASDL_copy_text(name, strlen(name)); if (!n->text) { p->error = "out of memory"; return NULL; } }
    return n;
}
static inline struct ASDLParsecNode *ASDLParsec_lookup(const struct ASDLParsec *p, const char *name) {
    if (p && name) for (struct ASDLParsecRule *r = p->rules; r; r = r->next) if (!strcmp(name, r->name)) return r->node;
    return NULL;
}
static inline bool ASDLParsec_rule(struct ASDLParsec *p, const char *name, struct ASDLParsecNode *node) {
    if (!p || !name || !node || p->error) return false;
    if (ASDLParsec_lookup(p, name)) { p->error = "duplicate rule"; return false; }
    struct ASDLParsecRule *r = (struct ASDLParsecRule *)ASDL_MALLOC(sizeof(*r));
    if (!r) { p->error = "out of memory"; return false; }
    r->name = ASDL_copy_text(name, strlen(name));
    if (!r->name) { ASDL_FREE(r); p->error = "out of memory"; return false; }
    r->node = node; r->next = p->rules; p->rules = r;
    if (!p->root) p->root = node;
    return true;
}
/* Ordered choice and greedy, possessive repetition (PEG semantics). A bounded
 * step budget and depth bound also terminate left-recursive/cyclic grammars. */
static inline int ASDLParsec_run(struct ASDLParsec *p, const struct ASDLParsecNode *n,
        const unsigned char *input, size_t length, size_t *position, size_t depth, size_t *steps) {
    if (!n) { p->error = "undefined rule"; return -1; }
    if (!depth || !*steps) { p->error = "parser resource limit"; return -1; }
    --*steps;
    size_t start = *position;
    int result = 0;
    switch (n->kind) {
    case ASDL_PARSEC_LITERAL:
        if (n->length <= length - start && (!n->length || !memcmp(input + start, n->text, n->length))) { *position += n->length; return 1; }
        break;
    case ASDL_PARSEC_RANGE:
        if (start < length && input[start] >= n->low && input[start] <= n->high) { ++*position; return 1; }
        break;
    case ASDL_PARSEC_ANY: if (start < length) { ++*position; return 1; } break;
    case ASDL_PARSEC_END: if (start == length) return 1; break;
    case ASDL_PARSEC_REF:
        return ASDLParsec_run(p, ASDLParsec_lookup(p, n->text), input, length, position, depth - 1, steps);
    case ASDL_PARSEC_SEQUENCE:
        result = ASDLParsec_run(p, n->left, input, length, position, depth - 1, steps);
        if (result == 1) result = ASDLParsec_run(p, n->right, input, length, position, depth - 1, steps);
        if (result == 1) return 1;
        *position = start;
        return result;
    case ASDL_PARSEC_CHOICE:
        result = ASDLParsec_run(p, n->left, input, length, position, depth - 1, steps);
        if (result != 0) return result;
        *position = start;
        return ASDLParsec_run(p, n->right, input, length, position, depth - 1, steps);
    case ASDL_PARSEC_REPEAT: {
        size_t count = 0;
        while (count < n->maximum) {
            size_t before = *position;
            result = ASDLParsec_run(p, n->left, input, length, position, depth - 1, steps);
            if (result < 0) { *position = start; return -1; }
            if (!result) { *position = before; break; }
            ++count;
            if (*position == before) {
                if (n->maximum != SIZE_MAX) return 1;
                p->error = "repetition matched empty input"; *position = start; return -1;
            }
        }
        if (count >= n->minimum) return 1;
        *position = start;
        break;
    }
    default: p->error = "invalid parser node"; return -1;
    }
    if (start > p->error_offset) p->error_offset = start;
    return 0;
}
static inline int ASDLParsec_match(struct ASDLParsec *p, const void *input, size_t length, bool complete, size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!p || !p->root || (length && !input)) return -1;
    p->error = NULL; p->error_offset = 0;
    size_t position = 0, steps = p->max_steps;
    int result = ASDLParsec_run(p, p->root, (const unsigned char *)input, length, &position, p->max_depth, &steps);
    if (result == 1 && complete && position != length) { p->error_offset = position; result = 0; }
    if (result == 1 && consumed) *consumed = position;
    if (!result) p->error = "input does not match";
    return result;
}
struct ASDLParsecGrammar { const char *text; size_t position; struct ASDLParsec *parser; };
static inline void ASDLParsec_space(struct ASDLParsecGrammar *g) {
    while (isspace((unsigned char)g->text[g->position])) ++g->position;
}
static inline char *ASDLParsec_name(struct ASDLParsecGrammar *g) {
    ASDLParsec_space(g);
    size_t start = g->position;
    if (!isalpha((unsigned char)g->text[start]) && g->text[start] != '_') return NULL;
    do { ++g->position; } while (isalnum((unsigned char)g->text[g->position]) || g->text[g->position] == '_');
    char *name = ASDL_copy_text(g->text + start, g->position - start);
    if (!name) g->parser->error = "out of memory";
    return name;
}
static inline struct ASDLParsecNode *ASDLParsec_expression(struct ASDLParsecGrammar *g, size_t depth);
static inline struct ASDLParsecNode *ASDLParsec_term(struct ASDLParsecGrammar *g, size_t depth) {
    ASDLParsec_space(g);
    struct ASDLParsec *p = g->parser;
    if (!depth) { p->error = "grammar nesting limit"; return NULL; }
    char c = g->text[g->position];
    struct ASDLParsecNode *n = NULL;
    if (c == '\'' || c == '"') {
        ++g->position;
        struct ASDLByteBuffer text = {0};
        while (g->text[g->position] && g->text[g->position] != c) {
            unsigned char ch = (unsigned char)g->text[g->position++];
            if (ch == '\\') {
                ch = (unsigned char)g->text[g->position];
                if (!ch) break;
                ++g->position;
                if (ch == 'n') ch = '\n'; else if (ch == 'r') ch = '\r'; else if (ch == 't') ch = '\t';
            }
            if (!ASDLByteBuffer_put(&text, ch)) { p->error = "out of memory"; break; }
        }
        if (g->text[g->position] != c) p->error = "unterminated literal";
        else { ++g->position; n = ASDLParsec_literal(p, text.data, text.length); }
        ASDLByteBuffer_destroy(&text);
    } else if (c == '(' || c == '[' || c == '{') {
        ++g->position;
        n = ASDLParsec_expression(g, depth - 1);
        ASDLParsec_space(g);
        char end = c == '(' ? ')' : c == '[' ? ']' : '}';
        if (g->text[g->position] != end) { p->error = "unclosed group"; return NULL; }
        ++g->position;
        if (c != '(') n = ASDLParsec_repeat(p, n, 0, c == '[' ? 1 : SIZE_MAX);
    } else if (c == '.') { ++g->position; n = ASDLParsec_any(p); }
    else {
        char *name = ASDLParsec_name(g);
        if (!name) { p->error = "expected expression"; return NULL; }
        n = ASDLParsec_ref(p, name); ASDL_FREE(name);
    }
    ASDLParsec_space(g);
    c = g->text[g->position];
    if (c == '*' || c == '+' || c == '?') {
        ++g->position;
        n = ASDLParsec_repeat(p, n, c == '+' ? 1 : 0, c == '?' ? 1 : SIZE_MAX);
    }
    return n;
}
static inline bool ASDLParsec_starts_term(char c) {
    return isalpha((unsigned char)c) || c == '_' || c == '\'' || c == '"' || c == '(' || c == '[' || c == '{' || c == '.';
}
static inline struct ASDLParsecNode *ASDLParsec_expression(struct ASDLParsecGrammar *g, size_t depth) {
    struct ASDLParsecNode *choice = NULL, *sequence = ASDLParsec_term(g, depth);
    while (sequence && !g->parser->error) {
        ASDLParsec_space(g);
        char c = g->text[g->position];
        if (c == ',' || ASDLParsec_starts_term(c)) {
            if (c == ',') ++g->position;
            struct ASDLParsecNode *right = ASDLParsec_term(g, depth);
            sequence = ASDLParsec_sequence(g->parser, sequence, right);
        } else if (c == '|') {
            ++g->position;
            choice = choice ? ASDLParsec_choice(g->parser, choice, sequence) : sequence;
            sequence = ASDLParsec_term(g, depth);
        } else break;
    }
    return choice ? ASDLParsec_choice(g->parser, choice, sequence) : sequence;
}
static inline bool ASDLParsec_compile(struct ASDLParsec *p, const char *grammar, const char *start_rule) {
    if (!p || !grammar) return false;
    struct ASDLParsec result; ASDLParsec_init(&result);
    result.max_depth = p->max_depth; result.max_steps = p->max_steps;
    struct ASDLParsecGrammar g = {grammar, 0, &result};
    ASDLParsec_space(&g);
    while (grammar[g.position] && !result.error) {
        char *name = ASDLParsec_name(&g);
        ASDLParsec_space(&g);
        if (!name || grammar[g.position] != '=') { ASDL_FREE(name); result.error = "expected rule = expression;"; break; }
        ++g.position;
        struct ASDLParsecNode *node = ASDLParsec_expression(&g, result.max_depth);
        ASDLParsec_space(&g);
        if (!node || grammar[g.position] != ';') { ASDL_FREE(name); if (!result.error) result.error = "expected semicolon"; break; }
        ++g.position;
        ASDLParsec_rule(&result, name, node); ASDL_FREE(name);
        ASDLParsec_space(&g);
    }
    if (!result.root && !result.error) result.error = "empty grammar";
    for (struct ASDLParsecNode *n = result.nodes; n && !result.error; n = n->next)
        if (n->kind == ASDL_PARSEC_REF && !ASDLParsec_lookup(&result, n->text)) result.error = "undefined rule";
    if (start_rule && !result.error) {
        result.root = ASDLParsec_lookup(&result, start_rule);
        if (!result.root) result.error = "undefined start rule";
    }
    if (result.error) {
        p->error = result.error; p->error_offset = g.position;
        ASDLParsec_destroy(&result); return false;
    }
    ASDLParsec_destroy(p); *p = result;
    return true;
}
#endif
)ASDL";

inline constexpr std::string_view listener = R"ASDL(#ifndef @U@LISTENER_H
#define @U@LISTENER_H
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
struct @P@listener {
    int fd, error;
    unsigned char *data;
    size_t length, position;
    struct ASDLEventHandler *handlers;
    size_t count, capacity, next_handler;
    struct ASDLEvent pending;
    bool has_pending, eof;
};
static inline void @P@listener_init(struct @P@listener *l) {
    if (l) { *l = (struct @P@listener){0}; l->fd = -1; }
}
static inline void @P@listener_close(struct @P@listener *l) {
    if (!l) return;
    if (l->data) munmap(l->data, l->length);
    if (l->fd >= 0) close(l->fd);
    ASDL_FREE(l->handlers); @P@listener_init(l);
}
static inline bool @P@listener_open(struct @P@listener *l, const char *path) {
    if (!l || !path) return false;
    @P@listener_init(l);
    l->fd = open(path, O_RDONLY);
    if (l->fd < 0) { l->error = errno; return false; }
    struct stat st;
    if (fstat(l->fd, &st) || !S_ISREG(st.st_mode)) {
        l->error = EINVAL; close(l->fd); l->fd = -1; return false;
    }
    return true;
}
/* Handlers may be changed between events, not from inside a callback or while
 * paused. All handlers see each event once, in registration order. */
static inline bool @P@listener_add(struct @P@listener *l, struct ASDLEventHandler handler) {
    if (!l || l->has_pending || !handler.callback) return false;
    if (l->count == l->capacity) {
        size_t capacity = l->capacity ? l->capacity * 2 : 4;
        if (capacity < l->capacity || capacity > SIZE_MAX / sizeof(handler)) return false;
        struct ASDLEventHandler *handlers = (struct ASDLEventHandler *)ASDL_MALLOC(capacity * sizeof(handler));
        if (!handlers) return false;
        if (l->count) memcpy(handlers, l->handlers, l->count * sizeof(handler));
        ASDL_FREE(l->handlers); l->handlers = handlers; l->capacity = capacity;
    }
    l->handlers[l->count++] = handler;
    return true;
}
static inline bool @P@listener_remove(struct @P@listener *l, size_t index) {
    if (!l || l->has_pending || index >= l->count) return false;
    memmove(l->handlers + index, l->handlers + index + 1, (l->count - index - 1) * sizeof(*l->handlers));
    --l->count;
    return true;
}
static inline int @P@listener_dispatch(struct @P@listener *l) {
    while (l->next_handler < l->count) {
        int action = ASDLEventHandler_call(&l->handlers[l->next_handler++], &l->pending);
        if (action) { if (action < 0) l->error = ECANCELED; return action; }
    }
    l->has_pending = false; l->next_handler = 0;
    return ASDL_EVENT_CONTINUE;
}
static inline int @P@listener_event(struct @P@listener *l, enum ASDLEventKind kind, size_t offset, size_t length) {
    l->pending = (struct ASDLEvent){kind, length ? l->data + offset : NULL, length, offset, 0};
    l->has_pending = true;
    return @P@listener_dispatch(l);
}
/* Poll an append-only file. DATA events are newline-terminated byte records
 * (newline included). final flushes a trailing record and sends EOF once per
 * file size. Truncation between polls resets the cursor and sends RESET.
 * The producer must not truncate or mutate mapped bytes during callbacks. */
static inline int @P@listener_poll(struct @P@listener *l, bool final) {
    if (!l || l->fd < 0 || l->error) return ASDL_EVENT_ERROR;
    if (l->has_pending) { int action = @P@listener_dispatch(l); if (action) return action; }
    struct stat st;
    if (fstat(l->fd, &st)) { l->error = errno; return ASDL_EVENT_ERROR; }
    if (st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) { l->error = EOVERFLOW; return ASDL_EVENT_ERROR; }
    size_t length = (size_t)st.st_size;
    if (length != l->length) {
        bool reset = length < l->length;
        unsigned char *data = NULL;
        if (length) {
            void *mapping = mmap(NULL, length, PROT_READ, MAP_SHARED, l->fd, 0);
            if (mapping == MAP_FAILED) { l->error = errno; return ASDL_EVENT_ERROR; }
            data = (unsigned char *)mapping;
        }
        if (l->data) munmap(l->data, l->length);
        l->data = data; l->length = length; l->eof = false;
        if (reset) { l->position = 0; int action = @P@listener_event(l, ASDL_EVENT_RESET, 0, 0); if (action) return action; }
    }
    while (l->position < l->length) {
        size_t start = l->position;
        const unsigned char *newline = (const unsigned char *)memchr(l->data + start, '\n', l->length - start);
        if (!newline && !final) break;
        size_t end = newline ? (size_t)(newline - l->data) + 1 : l->length;
        l->position = end;
        int action = @P@listener_event(l, ASDL_EVENT_DATA, start, end - start);
        if (action) return action;
    }
    if (final && !l->eof) { l->eof = true; return @P@listener_event(l, ASDL_EVENT_EOF, l->position, 0); }
    return ASDL_EVENT_CONTINUE;
}
#endif
)ASDL";

inline constexpr std::string_view runtime_types = R"ASDL(#ifndef @U@MALLOC
#define @U@MALLOC(size) malloc(size)
#endif
#ifndef @U@FREE
#define @U@FREE(pointer) free(pointer)
#endif

typedef enum @P@error {
    @U@OK = 0, @U@OOM, @U@INVALID, @U@OVERFLOW
} @P@error;
typedef enum @P@visit_result {
    @U@WALK_ERROR = -1, @U@WALK_CONTINUE = 0,
    @U@WALK_SKIP = 1, @U@WALK_STOP = 2
} @P@visit_result;

/* The union aligns block+1 for every type emitted by this generator. */
typedef struct @P@block {
    struct @P@block *next;
    max_align_t alignment;
} @P@block;
typedef struct @P@intern {
    struct @P@intern *next;
    char text[];
} @P@intern;
typedef struct @P@arena {
    @P@block *blocks;
    @P@intern *identifiers;
    size_t allocations;
    size_t bytes;
    @P@error error;
} @P@arena;

typedef struct @P@string { size_t length; const char *data; } @P@string_t;
typedef const char *@P@identifier_t;

)ASDL";

inline constexpr std::string_view runtime_functions = R"ASDL(FUNCTION void @P@arena_init(@P@arena *arena)
{
    if (arena) *arena = (@P@arena){0};
}
FUNCTION void @P@set_error(@P@arena *arena, @P@error error)
{
    if (arena && arena->error == @U@OK) arena->error = error;
}
FUNCTION void @P@arena_clear_error(@P@arena *arena)
{
    if (arena) arena->error = @U@OK;
}
FUNCTION const char *@P@error_string(@P@error error)
{
    switch (error) {
    case @U@OK: return "success";
    case @U@OOM: return "out of memory";
    case @U@INVALID: return "invalid argument";
    case @U@OVERFLOW: return "size overflow";
    default: return "unknown error";
    }
}
FUNCTION void *@P@arena_alloc(@P@arena *arena, size_t size)
{
    @P@block *block;
    if (!arena || arena->error != @U@OK) return NULL;
    if (size == 0) size = 1;
    if (size > SIZE_MAX - sizeof(*block) || size > SIZE_MAX - arena->bytes
        || arena->allocations == SIZE_MAX) {
        @P@set_error(arena, @U@OVERFLOW);
        return NULL;
    }
    block = (@P@block *)@U@MALLOC(sizeof(*block) + size);
    if (!block) {
        @P@set_error(arena, @U@OOM);
        return NULL;
    }
    block->next = arena->blocks;
    arena->blocks = block;
    arena->bytes += size;
    ++arena->allocations;
    return (void *)(block + 1);
}
FUNCTION void @P@arena_destroy(@P@arena *arena)
{
    @P@block *block;
    if (!arena) return;
    block = arena->blocks;
    while (block) {
        @P@block *next = block->next;
        @U@FREE(block);
        block = next;
    }
    @P@arena_init(arena);
}
FUNCTION @P@string_t @P@string_view(const char *data, size_t length)
{
    @P@string_t value = {length, data};
    return value;
}
FUNCTION @P@string_t @P@string_copy(@P@arena *arena, const char *data, size_t length)
{
    @P@string_t value = {0, NULL};
    char *copy;
    if (!arena || arena->error != @U@OK) return value;
    if (length && !data) { @P@set_error(arena, @U@INVALID); return value; }
    if (length == SIZE_MAX) { @P@set_error(arena, @U@OVERFLOW); return value; }
    copy = (char *)@P@arena_alloc(arena, length + 1);
    if (!copy) return value;
    if (length) memcpy(copy, data, length);
    copy[length] = '\0';
    value.length = length;
    value.data = copy;
    return value;
}
FUNCTION @P@identifier_t @P@identifier(@P@arena *arena, const char *text)
{
    @P@intern *entry;
    size_t length;
    if (!arena || arena->error != @U@OK) return NULL;
    if (!text) { @P@set_error(arena, @U@INVALID); return NULL; }
    for (entry = arena->identifiers; entry; entry = entry->next)
        if (strcmp(entry->text, text) == 0) return entry->text;
    length = strlen(text);
    if (length > SIZE_MAX - sizeof(*entry) - 1) {
        @P@set_error(arena, @U@OVERFLOW);
        return NULL;
    }
    entry = (@P@intern *)@P@arena_alloc(arena, sizeof(*entry) + length + 1);
    if (!entry) return NULL;
    entry->next = arena->identifiers;
    memcpy(entry->text, text, length + 1);
    arena->identifiers = entry;
    return entry->text;
}
)ASDL";

inline constexpr std::string_view visitor_begin = R"ASDL(#ifndef @U@VISITOR_NAME
#error "Define @U@VISITOR_NAME before including this visitor template"
#endif
#ifndef @U@VISITOR_CONTEXT
#define @U@VISITOR_CONTEXT void
#define @U@PRIVATE_DEFAULT_CONTEXT
#endif
#ifndef @U@VISITOR_MAX_DEPTH
#define @U@VISITOR_MAX_DEPTH 1024
#define @U@PRIVATE_DEFAULT_DEPTH
#endif
#define @U@PRIVATE_CAT_I(a, b) a##b
#define @U@PRIVATE_CAT(a, b) @U@PRIVATE_CAT_I(a, b)
#define @U@PRIVATE_FN(suffix) @U@PRIVATE_CAT(@U@VISITOR_NAME, suffix)

static inline int @U@PRIVATE_FN(__result)(int action)
{
    return action == @U@WALK_STOP ? @U@WALK_STOP : @U@WALK_ERROR;
}

)ASDL";

inline constexpr std::string_view visitor_end = R"ASDL(#undef @U@PRIVATE_FN
#undef @U@PRIVATE_CAT
#undef @U@PRIVATE_CAT_I
#ifdef @U@PRIVATE_DEFAULT_CONTEXT
#undef @U@VISITOR_CONTEXT
#undef @U@PRIVATE_DEFAULT_CONTEXT
#endif
#ifdef @U@PRIVATE_DEFAULT_DEPTH
#undef @U@VISITOR_MAX_DEPTH
#undef @U@PRIVATE_DEFAULT_DEPTH
#endif
)ASDL";

} // namespace asdl::codegen_detail
