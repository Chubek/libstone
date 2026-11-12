/* Minimal assertion support that stays active in Release builds. */
#ifndef MEMTKX_TEST_SUPPORT_H
#define MEMTKX_TEST_SUPPORT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

#define CHECK_STR_EQ(a, b) CHECK(strcmp((a), (b)) == 0)

#endif
