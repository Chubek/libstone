#include "ld.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "%d: check failed: %s\n", __LINE__, #expr); \
  exit(1); \
} } while (0)

static void expect_manifest(const char *source, size_t source_size, const char *expected) {
  uint8_t *bytes = NULL;
  size_t size = 0;
  char *error = NULL;
  int status = qld_backend_manifest(source, source_size, &bytes, &size, &error);
  if (error) fprintf(stderr, "%s\n", error);
  CHECK(status == QLD_OK && !error && bytes);
  CHECK(size == strlen(expected) && !memcmp(bytes, expected, size));
  qld_free_bytes(bytes, size);
}

static void expect_error(const char *source, size_t source_size,
                         int expected_status, const char *message) {
  uint8_t dummy = 0;
  uint8_t *bytes = &dummy;
  size_t size = 42;
  char *error = NULL;
  int status = qld_backend_manifest(source, source_size, &bytes, &size, &error);
  if (status != expected_status || !error || !strstr(error, message)) {
    fprintf(stderr, "expected status %d (%s), got %d (%s)\n",
        expected_status, message, status, error ? error : "no diagnostic");
    exit(1);
  }
  CHECK(!bytes && size == 0);
  qld_free_string(error);
}

#define REJECT(source, status, message) \
  expect_error(source, sizeof(source) - 1, status, message)
#define MALFORMED(source, message) REJECT(source, QLD_PARSE_ERROR, message)

int main(void) {
  const char valid[] =
      "--!lua\n"
      "local specs = {{'mir-c11', '_Register2'}, {'c11', 'on1x_register_c11'}}\n"
      "for _, spec in ipairs(specs) do qld.backend(spec[1], spec[2]) end\n";
  const char expected[] = "mir-c11\t_Register2\nc11\ton1x_register_c11\n";
  expect_manifest(valid, sizeof(valid) - 1, expected);
  /* Source need not be terminated, and unrelated bytes beyond source_size
     must never be evaluated (even if they would register another backend). */
  const char slices[] = "qld.backend('a', 'R')qld.backend('b', 'S')";
  expect_manifest(slices, sizeof("qld.backend('a', 'R')") - 1, "a\tR\n");

  char *error = NULL;
  size_t size = 7;
  uint8_t dummy = 0, *bytes = &dummy;
  CHECK(qld_backend_manifest(valid, sizeof(valid) - 1, NULL, &size, &error) ==
        QLD_INVALID_ARGUMENT);
  CHECK(!size && error);
  qld_free_string(error);
  CHECK(qld_backend_manifest(valid, sizeof(valid) - 1, &bytes, NULL, NULL) ==
        QLD_INVALID_ARGUMENT);
  CHECK(!bytes);
  expect_error(NULL, 0, QLD_INVALID_ARGUMENT, "null");
  expect_error(valid, SIZE_MAX, QLD_ERROR, "source size limit");

  MALFORMED("", "registered no backends");
  MALFORMED("return 'artifact'", "registered no backends");
  MALFORMED("qld.backend(", "backend manifest:1:");
  MALFORMED("qld.backend('first', 'R'); error('later failure')", "later failure");
  MALFORMED("error({})", "backend manifest Lua error");
  MALFORMED("qld.backend('a', 'R')\0qld.backend('b', 'S')", "NUL in");
  MALFORMED("\x1b" "Lua", "binary chunk");
  MALFORMED("qld.backend('a')", "exactly two strings");
  MALFORMED("qld.backend('a', 'R', 'extra')", "exactly two strings");
  MALFORMED("qld.backend('a', 17)", "exactly two strings");
  MALFORMED("qld.backend({}, 'R')", "exactly two strings");
  MALFORMED("qld.backend('', 'R')", "invalid backend name");
  MALFORMED("qld.backend('A', 'R')", "invalid backend name");
  MALFORMED("qld.backend('9', 'R')", "invalid backend name");
  MALFORMED("qld.backend('a_b', 'R')", "invalid backend name");
  MALFORMED("qld.backend('a\\tname', 'R')", "invalid backend name");
  MALFORMED("qld.backend('a\\0name', 'R')", "invalid backend name");
  MALFORMED("qld.backend('a\\255', 'R')", "invalid backend name");
  MALFORMED("qld.backend('a', '')", "invalid backend registrar");
  MALFORMED("qld.backend('a', '3R')", "invalid backend registrar");
  MALFORMED("qld.backend('a', 'R\\nother')", "invalid backend registrar");
  MALFORMED("qld.backend('a', 'R\\0name')", "invalid backend registrar");
  MALFORMED("qld.backend('a', 'R();')", "invalid backend registrar");
  MALFORMED("qld.backend('a', 'R'); qld.backend('a','S')", "duplicate backend name");
  MALFORMED("qld.backend('a', 'R'); qld.backend('b','R')", "duplicate backend registrar");
  MALFORMED("qld.backend('a', 'R'); qld.emit('')", "field 'emit'");
  MALFORMED("qld.backend('a', 'R'); io.open('ignored')", "global 'io'");
  MALFORMED("qld.backend('a', 'R'); os.execute('ignored')", "global 'os'");
  MALFORMED("qld.backend('a', 'R'); dofile('ignored')", "global 'dofile'");
  MALFORMED("qld.backend('a', 'R'); loadfile('ignored')", "global 'loadfile'");
  MALFORMED("qld.backend('a', 'R'); load('return 1')", "global 'load'");
  MALFORMED("qld.backend('a', 'R'); require('ignored')", "global 'require'");
  MALFORMED("qld.backend('a', 'R'); package.loadlib('ignored')", "global 'package'");
  MALFORMED("qld.backend('a', 'R'); debug.sethook()", "global 'debug'");
  MALFORMED("qld.backend('a', 'R'); coroutine.create(function() end)", "global 'coroutine'");
  MALFORMED("qld.backend('a', 'R'); pcall(qld.backend, 'bad name', 'X')", "global 'pcall'");
  MALFORMED("local s=''; for i=1,65 do s=s..'a' end qld.backend(s,'R')", "name length limit");
  MALFORMED("local s=''; for i=1,129 do s=s..'A' end qld.backend('a',s)", "registrar length limit");
  MALFORMED("for i=1,129 do qld.backend('a'..i, 'R'..i) end", "registration limit");
  REJECT("qld.backend('a','R'); while true do end", QLD_ERROR, "instruction limit");
  REJECT("qld.backend('a','R'); local s='a'; for i=1,24 do s=s..s end",
      QLD_OUT_OF_MEMORY, "allocation limit");

  char max_source[65536];
  memset(max_source, ' ', sizeof(max_source));
  memcpy(max_source, slices, sizeof("qld.backend('a', 'R')") - 1);
  expect_manifest(max_source, sizeof(max_source), "a\tR\n");
  char max_record[64 + 1 + 128 + 1 + 1];
  memset(max_record, 'a', 64);
  max_record[64] = '\t';
  memset(max_record + 65, 'R', 128);
  max_record[193] = '\n';
  max_record[194] = 0;
  const char longest[] =
      "local n=''; for i=1,64 do n=n..'a' end "
      "local r=''; for i=1,128 do r=r..'R' end qld.backend(n,r)";
  expect_manifest(longest, sizeof(longest) - 1, max_record);

  char max_records[2048];
  size_t pos = 0;
  for (int i = 1; i <= 128; ++i) {
    const int written = snprintf(max_records + pos, sizeof(max_records) - pos, "a%d\tR%d\n", i, i);
    CHECK(written > 0 && (size_t)written < sizeof(max_records) - pos);
    pos += (size_t)written;
  }
  const char max_count[] = "for i=1,128 do qld.backend('a'..i, 'R'..i) end";
  expect_manifest(max_count, sizeof(max_count) - 1, max_records);
  /* Calls start with no prior successful or failed registrations. */
  expect_manifest(valid, sizeof(valid) - 1, expected);
  return 0;
}
