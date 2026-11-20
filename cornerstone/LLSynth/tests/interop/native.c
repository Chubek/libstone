#include <stdint.h>
#include <stddef.h>

int64_t interop_add(int64_t a, int64_t b) { return a + b; }
double interop_scale(double a, double b) { return a * b; }
int interop_not(int a) { return !a; }
const char *interop_echo(const char *s) { return s; }
const char *interop_null(void) { return NULL; }
const char *interop_invalid(void) { return "\xff"; }
static int64_t recorded;
void interop_record(int64_t value) { recorded = value; }
int64_t interop_read(void) { return recorded; }
