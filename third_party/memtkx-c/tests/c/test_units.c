/* Port of tests/test_units.cpp plus runtime literal parsing. */
#include <memtkx/memtkx.h>

#include "test_support.h"

/* MTKX_KB etc. are integer constant expressions, like the C++ literals. */
typedef char kb_is_constant[MTKX_KB(1) == 1024 ? 1 : -1];
typedef char mb_is_constant[MTKX_MB(1) == 1024 * 1024 ? 1 : -1];

int main(void) {
  mtkx_address_range range = mtkx_range(100, 200);
  long double parsed = 0.0L;

  CHECK(MTKX_KB(1) == 1024);
  CHECK(MTKX_MB(1) == 1024 * 1024);
  CHECK((unsigned long long)MTKX_GB(1) == 1024ULL * 1024ULL * 1024ULL);
  CHECK(MTKX_B(64) == 64);

  CHECK(mtkx_is_power_of_two(8));
  CHECK(!mtkx_is_power_of_two(6));
  CHECK(!mtkx_is_power_of_two(0));

  CHECK(mtkx_align_up(17, 8) == 24);
  CHECK(mtkx_align_down(17, 8) == 16);
  CHECK(mtkx_align_up(17, 1) == 17);

  CHECK(mtkx_range_contains(range, 100));
  CHECK(!mtkx_range_contains(range, 200));
  CHECK(mtkx_range_size(range) == 100);
  CHECK(mtkx_range_size(mtkx_range(200, 100)) == 0);
  CHECK(mtkx_range_empty(mtkx_range(5, 5)));
  CHECK(mtkx_range_contains_range(range, mtkx_range(120, 150)));
  CHECK(mtkx_range_overlaps(range, mtkx_range(150, 250)));
  CHECK(!mtkx_range_overlaps(range, mtkx_range(200, 250)));
  CHECK(mtkx_range_clamp(range, 50) == 100);
  CHECK(mtkx_range_clamp(range, 500) == 200);
  CHECK(mtkx_range_clamp(range, 150) == 150);
  CHECK(mtkx_range_equal(range, mtkx_range(100, 200)));

  CHECK(mtkx_bytes_to_kib(2048) == 2);
  CHECK(mtkx_kib_to_bytes(2) == 2048);
  CHECK(mtkx_bytes_to_mib(3 * 1024 * 1024 + 5) == 3);
  CHECK(mtkx_mib_to_bytes(2) == 2 * 1024 * 1024);

  CHECK(mtkx_can_allocate(0, 100, 100, 1));
  CHECK(!mtkx_can_allocate(0, 100, 101, 1));
  CHECK(!mtkx_can_allocate(100, 100, 1, 1));
  CHECK(!mtkx_can_allocate(97, 100, 1, 8)); /* aligned start passes the limit */

  CHECK(MTKX_MAX_ALIGN >= sizeof(void *));
  CHECK(mtkx_is_power_of_two(MTKX_MAX_ALIGN));
  CHECK(MTKX_ALIGNOF(double) <= MTKX_MAX_ALIGN);

  CHECK(mtkx_units_parse_literal("1.5_MB", &parsed) == MTKX_OK);
  CHECK(parsed == 1.5L * 1024.0L * 1024.0L);
  CHECK(mtkx_units_parse_literal("64_KB", &parsed) == MTKX_OK && parsed == 65536.0L);
  CHECK(mtkx_units_parse_literal("2_GB", &parsed) == MTKX_OK &&
        parsed == 2.0L * 1024.0L * 1024.0L * 1024.0L);
  CHECK(mtkx_units_parse_literal("0.5_B", &parsed) == MTKX_OK && parsed == 0.0L);
  CHECK(mtkx_units_parse_literal("10", &parsed) == MTKX_ERR_NOT_FOUND);
  CHECK(mtkx_units_parse_literal("abc_KB", &parsed) == MTKX_ERR_PARSE);

  CHECK(mtkx_version() == MTKX_VERSION);
  CHECK_STR_EQ(mtkx_status_string(MTKX_ERR_NOMEM), "out of native memory");
  return 0;
}
