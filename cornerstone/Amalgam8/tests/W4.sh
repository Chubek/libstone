#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/amalgam8-W4.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic "$test_dir/W4.cpp" -o "$work/generate"
"$work/generate" "$work"
cat > "$work/a.c" <<'EOF'
#include "api.h"
#include "api.h"

#ifdef AMALGAM8_W4Emitter_UNUSED
#error emitter annotation helper escaped the API guard
#endif

int external_value = 2;
int increment(int value) { return value + external_value; }
#ifdef __cplusplus
extern "C"
#endif
int call_a(void) { return call_counter() + get_callback()(0); }
EOF
cat > "$work/b.c" <<'EOF'
#include "api.h"
#include "api.h"

#ifdef __cplusplus
extern "C"
#endif
int call_a(void);

int main(void) {
    int row[3] = {4, 0, 0};
    Node n = {NULL, {5, 0, 0}, &row, {get_callback(), NULL}};
    const char *bytes = escaped();
    return call_a() != 43 || call_counter() != 41 || external_value != 2 || evaluate(&n) != 5 ||
           n.callbacks[0]((*n.row_ptr)[0] + n.row[0]) != 11 || READY != 1 ||
           bytes[0] != '?' || bytes[1] != '?' || bytes[2] != '/' || bytes[3] != '"' ||
           bytes[4] != '\\' || bytes[5] != '\n' || bytes[6] != 1 || bytes[7] != '7';
}
EOF
cat > "$work/externs.c" <<'EOF'
int external_value;
int increment(int value) { return value + external_value; }
EOF

c_flags='-std=c11 -Wall -Wextra -Werror -pedantic-errors'
cpp_flags="-std=${W4_CPPSTD:-c++11} -Wall -Wextra -Werror -pedantic-errors"
"${CC:-cc}" $c_flags "$work/a.c" "$work/b.c" -o "$work/two-c"
"$work/two-c"
"${CC:-cc}" $c_flags -c "$work/a.c" -o "$work/a.o"
"${CXX:-c++}" $cpp_flags -x c++ -c "$work/b.c" -o "$work/b.o"
"${CXX:-c++}" "$work/a.o" "$work/b.o" -o "$work/two-mixed"
"$work/two-mixed"
"${CXX:-c++}" $cpp_flags -x c++ -c "$work/a.c" -o "$work/a-cpp.o"
"${CXX:-c++}" "$work/a-cpp.o" "$work/b.o" -o "$work/two-cpp"
"$work/two-cpp"

"${CC:-cc}" $c_flags -c "$work/externs.c" -o "$work/externs.o"
for source in "$work/cli_raw.c" "$work/cli_entry.c"; do
    "${CC:-cc}" $c_flags "$source" "$work/externs.o" -o "$work/cli-c"
    "$work/cli-c"
    "${CXX:-c++}" $cpp_flags -x c++ -c "$source" -o "$work/cli.o"
    "${CXX:-c++}" "$work/cli.o" "$work/externs.o" -o "$work/cli-cpp"
    "$work/cli-cpp"
done

cat > "$work/include_cli.c" <<'EOF'
#include "cli_entry.h"
#include "cli_entry.c"
#include "cli_entry.c"
EOF
"${CC:-cc}" $c_flags "$work/include_cli.c" "$work/externs.o" -o "$work/include-cli"
"$work/include-cli"
