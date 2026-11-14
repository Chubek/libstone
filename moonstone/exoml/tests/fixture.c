#include "fixture.h"
#include <string.h>
static int stored;
int exoml_add(int a, int b) { return a + b; }
double exoml_half(double value) { return value / 2; }
const char *exoml_echo(const char *value) { return value; }
size_t exoml_length(const char *value) { return strlen(value); }
unsigned long long exoml_wide(unsigned long long value) { return value; }
void exoml_set(int value) { stored = value; }
int exoml_get(void) { return stored; }
