#ifndef EXOML_TEST_FIXTURE_H
#define EXOML_TEST_FIXTURE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct exoml_pair {
    char tag;
    int value;
};
int exoml_add(int a, int b);
double exoml_half(double value);
const char *exoml_echo(const char *value);
size_t exoml_length(const char *value);
unsigned long long exoml_wide(unsigned long long value);
void exoml_set(int value);
int exoml_get(void);
#ifdef __cplusplus
}
#endif
#endif
