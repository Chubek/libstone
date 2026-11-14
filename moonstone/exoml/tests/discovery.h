#ifndef EXOML_DISCOVERY_H
#define EXOML_DISCOVERY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define EXOML_NUMBER 42
#define EXOML_INCREMENT(x) ((x) + 1)
typedef unsigned long exoml_word;
typedef exoml_word exoml_alias;
typedef int (*exoml_callback)(const char *, size_t, void *);
typedef struct exoml_opaque exoml_opaque;
struct exoml_forward;
typedef struct exoml_forward *exoml_forward_pointer;
struct exoml_forward {
    int resolved;
};
typedef struct exoml_anonymous {
    union {
        int integer;
        double real;
    };
    unsigned char bytes[];
} exoml_anonymous;
typedef struct exoml_node {
    struct exoml_node *next;
    const volatile int *pointer;
    int values[7];
    unsigned flags : 3;
    union {
        double real;
        intptr_t integer;
    } payload;
} exoml_node;
struct __attribute__((packed)) exoml_packed {
    char tag;
    long value;
};
struct __attribute__((aligned(32))) exoml_aligned {
    int value;
};
enum exoml_mode { EXOML_OFF = -1, EXOML_ON = 9 };
int exoml_call(exoml_alias count, exoml_callback callback, exoml_opaque *owner);
void exoml_array(const unsigned char bytes[32], int *restrict result);
bool exoml_bool(bool flag);
const exoml_node *exoml_const_node(const exoml_node *node);
static inline long exoml_inline(long value) { return value; }
int exoml_variadic(int first, ...);
int exoml_unprototyped();
#ifdef EXOML_ALTERNATE
int exoml_conditional(exoml_alias value);
#endif
#ifdef EXOML_INVALID
this is deliberately invalid C;
#endif
#ifdef EXOML_IMPLEMENTATION
#define MULIB_IMPL_FORBIDDEN
#endif
#endif
