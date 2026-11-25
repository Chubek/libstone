set(dir "${BINARY}/module-c90")
file(MAKE_DIRECTORY "${dir}")
separate_arguments(cflags NATIVE_COMMAND "${CFLAGS}")
separate_arguments(link_flags NATIVE_COMMAND "${LINK_FLAGS}")

function(run_ok)
  execute_process(${ARGN} RESULT_VARIABLE status ERROR_VARIABLE error)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "command failed (${status}): ${ARGN}\n${error}")
  endif()
endfunction()

file(WRITE "${dir}/module.wat" [=[
(module
  (type $counter (func (param i32 i32) (result i32)))
  (type $pass (func (param i32) (result i32)))
  (func $pair (param i64) (result i64 i32)
    local.get 0
    i64.const 3
    i64.div_s
    i32.const 42)
  (func (export "run") (param $n i32) (result i64 i32)
    (local $sum i32)
    block $done
      loop $next
        local.get $n
        i32.eqz
        br_if $done
        local.get $n
        local.get $sum
        i32.add
        local.set $sum
        local.get $n
        i32.const 1
        i32.sub
        local.set $n
        br $next
      end
    end
    local.get $sum
    i64.extend_i32_u
    call $pair)
  (func (export "table") (param i32) (result i32)
    block $outer (result i32)
      block $inner (result i32)
        i32.const 31
        local.get 0
        br_table $inner $outer
      end
      i32.const 11
      i32.add
    end)
  (func (export "choose") (param i32) (result i32)
    local.get 0
    if (result i32)
      i32.const 42
      return
    else
      i32.const 7
    end)
  (func (export "divide") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.div_s)
  (func (export "quotient") (param i64 i64) (result i64 i64)
    local.get 0 local.get 1 i64.div_u
    local.get 0 local.get 1 i64.rem_u)
  (func (export "remainder") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.rem_s)
  (func (export "typed-loop") (param $n i32) (result i32)
    block $done (result i32)
      i32.const 0
      local.get $n
      loop $next (type $counter)
        local.set $n
        local.get $n
        i32.eqz
        br_if $done
        local.get $n
        i32.add
        local.get $n
        i32.const 1
        i32.sub
        br $next
      end
    end)
  (func (export "maybe") (param i32 i32) (result i32)
    local.get 0 local.get 1
    if (type $pass)
      i32.const 1 i32.add
    end))
]=])
file(WRITE "${dir}/driver.c" [=[
#include <stddef.h>
struct wasm_result { unsigned width; unsigned long lo, hi; };
int wasm_module_invoke(const char *, size_t, const struct wasm_result *, size_t,
                       struct wasm_result *, size_t, size_t *, unsigned long);

int main(void)
{
    struct wasm_result values[2] = {{32, 6UL, 0UL}, {0, 0UL, 0UL}};
    size_t count;
    if (wasm_module_invoke("run", 3, values, 1, values, 2, &count, 1000) ||
        count != 2 || values[0].width != 64 || values[0].lo != 7UL ||
        values[0].hi != 0UL || values[1].width != 32 || values[1].lo != 42UL)
        return 1;
    if (wasm_module_invoke("run", 3, values + 1, 1, values, 2, &count, 1) != 1 ||
        count != 0 || values[0].lo != 7UL)
        return 2;
    values[0].width = 32;
    values[0].lo = 0UL;
    if (wasm_module_invoke("table", 5, values, 1, values, 1, &count, 1000) ||
        values[0].lo != 42UL ||
        wasm_module_invoke("table", 5, values, 1, values, 1, &count, 1000) ||
        values[0].lo != 31UL)
        return 3;
    if (wasm_module_invoke("choose", 6, values, 1, values, 1, &count, 1000) ||
        values[0].lo != 42UL)
        return 4;
    values[0].lo = 0UL;
    if (wasm_module_invoke("choose", 6, values, 1, NULL, 0, &count, 1000) != 2 ||
        count != 1)
        return 5;
    if (wasm_module_invoke("choose", 6, values, 1, values, 1, &count, 1000) ||
        values[0].lo != 7UL)
        return 6;
    if (wasm_module_invoke("unknown", 7, NULL, 0, NULL, 0, &count, 1000) != 3 ||
        count != 0)
        return 7;
    values[0].width = values[1].width = 64;
    values[0].hi = 0x80000000UL;
    values[0].lo = 0UL;
    values[1].hi = values[1].lo = 0xffffffffUL;
    if (wasm_module_invoke("divide", 6, values, 2, values, 1, &count, 1000) != 1 ||
        count != 0 || values[0].hi != 0x80000000UL)
        return 8;
    values[0].hi = values[0].lo = 0xffffffffUL;
    values[1].hi = 1UL;
    values[1].lo = 1UL;
    if (wasm_module_invoke("quotient", 8, values, 2, values, 2, &count, 1000) ||
        count != 2 || values[0].hi != 0UL || values[0].lo != 0xffffffffUL ||
        values[1].hi != 0UL || values[1].lo != 0UL)
        return 9;
    values[0].width = values[1].width = 32;
    values[0].hi = values[1].hi = 0UL;
    values[0].lo = 0xfffffff9UL;
    values[1].lo = 3UL;
    if (wasm_module_invoke("remainder", 9, values, 2, values, 1, &count, 1000) ||
        count != 1 || values[0].lo != 0xffffffffUL || values[0].hi != 0UL)
        return 10;
    values[0].lo = 6UL;
    if (wasm_module_invoke("typed-loop", 10, values, 1, values, 1, &count, 1000) ||
        count != 1 || values[0].lo != 21UL)
        return 11;
    values[1].lo = 0UL;
    if (wasm_module_invoke("maybe", 5, values, 2, values, 1, &count, 1000) || values[0].lo != 21UL)
        return 12;
    values[1].lo = 1UL;
    if (wasm_module_invoke("maybe", 5, values, 2, values, 1, &count, 1000) || values[0].lo != 22UL)
        return 13;
    return 0;
}
]=])

run_ok(COMMAND "${ASSEMBLE}" "${dir}/module.wat" OUTPUT_FILE "${dir}/module.wasm")
run_ok(COMMAND "${WASM2C}" "${dir}/module.wasm" OUTPUT_FILE "${dir}/from_wasm.c")
run_ok(COMMAND "${WAT2C}" --module "${dir}/module.wat" OUTPUT_FILE "${dir}/from_wat.c")
run_ok(COMMAND "${CMAKE_COMMAND}" -E compare_files "${dir}/from_wasm.c" "${dir}/from_wat.c")
run_ok(COMMAND "${CC}" ${cflags} -std=c90 -pedantic-errors -Wall -Wextra -Werror
  "${dir}/from_wasm.c" "${dir}/driver.c" ${link_flags} -o "${dir}/driver${EXE_SUFFIX}")
run_ok(COMMAND "${dir}/driver${EXE_SUFFIX}")

file(WRITE "${dir}/state.wat" [=[
(module
  (memory (export "mem") 1 2)
  (global $counter (export "counter") (mut i32) (i32.const 5))
  (global (export "const") f64 (f64.const -0))
  (data (i32.const 0) "\ff\80")
  (data $passive "Wasm")
  (func $start
    i32.const 6 global.set $counter
    i32.const 2 i32.const 0x12345678 i32.store)
  (start $start)
  (func (export "advance") (result i32)
    global.get $counter i32.const 1 i32.add global.set $counter
    global.get $counter)
  (func (export "read") (result i64)
    i32.const 1 i64.load8_s)
  (func (export "write-and-trap")
    i32.const 1 i32.const 13 i32.store8
    unreachable)
  (func (export "overflowing-offset") (result i32)
    i32.const -1 i32.load offset=2)
  (func (export "bulk")
    i32.const 10 i32.const 0 i32.const 4 memory.init $passive
    i32.const 11 i32.const 10 i32.const 4 memory.copy
    i32.const 14 i32.const 0x12a i32.const 2 memory.fill
    data.drop $passive)
  (func (export "empty-init")
    i32.const 65536 i32.const 0 i32.const 0 memory.init $passive)
  (func (export "grow") (result i32 i32)
    i32.const 1 memory.grow memory.size)
  (func (export "divide") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.div_s)
  (func (export "float") (param f32) (result f32 f32 f64 f64 f64)
    local.get 0 f32.const 2 f32.mul
    f32.const -nan:0x1234 f32.abs
    f64.const 0 f64.const -0 f64.min
    f64.const -0.5 f64.nearest
    f64.const 2.5 f64.nearest)
  (func (export "convert") (result f32 f64 i32 i64 i64)
    i64.const 0x8000008000000001 f32.convert_i64_u
    i64.const -1 f64.convert_i64_u
    f64.const -2147483648.75 i32.trunc_f64_s
    f64.const inf i64.trunc_sat_f64_u
    f32.const nan i64.trunc_sat_f32_s)
  (func (export "trunc") (param f64) (result i64)
    local.get 0 i64.trunc_f64_s))
]=])
file(WRITE "${dir}/state-driver.c" [=[
#include <stddef.h>
#include <string.h>
struct wasm_result { unsigned width; unsigned long lo, hi; };
struct wasm_instance;
int wasm_instance_create(struct wasm_instance **, unsigned long);
void wasm_instance_destroy(struct wasm_instance *);
int wasm_instance_invoke(struct wasm_instance *, const char *, size_t,
                         const struct wasm_result *, size_t, struct wasm_result *,
                         size_t, size_t *, unsigned long);
int wasm_instance_read_memory(struct wasm_instance *, const char *, size_t,
                              unsigned long, unsigned char *, size_t);
int wasm_instance_write_memory(struct wasm_instance *, const char *, size_t,
                               unsigned long, const unsigned char *, size_t);
int wasm_instance_get_global(struct wasm_instance *, const char *, size_t, struct wasm_result *);
int wasm_instance_set_global(struct wasm_instance *, const char *, size_t, const struct wasm_result *);

#define CALL(instance, name, args, nargs, capacity) \
    wasm_instance_invoke(instance, name, sizeof(name) - 1U, args, nargs, \
                         values, capacity, &count, 1000UL)

int main(void)
{
    struct wasm_instance *a, *b;
    struct wasm_result values[5], args[2];
    size_t count;
    unsigned char bytes[6];
    if (wasm_instance_create(&a, 0UL) != 7 || a != NULL ||
        wasm_instance_create(&a, 6UL) || wasm_instance_create(&b, 1000UL))
        return 1;
    if (wasm_instance_read_memory(a, "mem", 3, 0UL, bytes, sizeof(bytes)) ||
        memcmp(bytes, "\377\200\170\126\064\022", sizeof(bytes)) != 0 ||
        CALL(a, "read", NULL, 0, 1) || count != 1 ||
        values[0].lo != 0xffffff80UL || values[0].hi != 0xffffffffUL)
        return 2;
    if (CALL(a, "advance", NULL, 0, 1) || values[0].lo != 7UL ||
        CALL(a, "advance", NULL, 0, 0) != 2 || count != 1 ||
        wasm_instance_get_global(a, "counter", 7, values) || values[0].lo != 8UL ||
        wasm_instance_get_global(b, "counter", 7, values) || values[0].lo != 6UL ||
        wasm_instance_get_global(a, "const", 5, values + 1) || values[1].hi != 0x80000000UL ||
        wasm_instance_set_global(a, "const", 5, values + 1) != 3)
        return 3;
    values[0].lo = 41UL;
    if (wasm_instance_set_global(a, "counter", 7, values) ||
        CALL(a, "advance", NULL, 0, 1) || values[0].lo != 42UL)
        return 4;
    if (CALL(a, "write-and-trap", NULL, 0, 0) != 1 || count != 0 ||
        CALL(a, "read", NULL, 0, 1) || values[0].lo != 13UL ||
        CALL(a, "overflowing-offset", NULL, 0, 1) != 6 || count != 0 || values[0].lo != 13UL ||
        wasm_instance_write_memory(a, "mem", 3, 65536UL, bytes, 1) != 6)
        return 5;
    if (CALL(a, "bulk", NULL, 0, 0) ||
        wasm_instance_read_memory(a, "mem", 3, 10UL, bytes, sizeof(bytes)) ||
        memcmp(bytes, "WWas**", sizeof(bytes)) != 0 ||
        CALL(a, "bulk", NULL, 0, 0) != 6 ||
        CALL(a, "empty-init", NULL, 0, 0) ||
        CALL(b, "bulk", NULL, 0, 0))
        return 6;
    if (CALL(a, "grow", NULL, 0, 2) || values[0].lo != 1UL || values[1].lo != 2UL ||
        CALL(a, "grow", NULL, 0, 2) || values[0].lo != 0xffffffffUL || values[1].lo != 2UL ||
        wasm_instance_read_memory(a, "mem", 3, 65536UL, bytes, sizeof(bytes)) ||
        memcmp(bytes, "\0\0\0\0\0", sizeof(bytes)) != 0)
        return 7;
    args[0].width = args[1].width = 32U;
    args[0].lo = 0x80000000UL; args[0].hi = args[1].hi = 0UL; args[1].lo = 0UL;
    if (CALL(a, "divide", args, 2, 1) != 4 || count != 0)
        return 8;
    args[1].lo = 0xffffffffUL;
    if (CALL(a, "divide", args, 2, 1) != 5 || count != 0)
        return 9;
    args[0].lo = 0x3fc00000UL;
    if (CALL(a, "float", args, 1, 5) || count != 5 ||
        values[0].width != 32U || values[0].lo != 0x40400000UL ||
        values[1].lo != 0x7f801234UL || values[2].hi != 0x80000000UL ||
        values[3].hi != 0x80000000UL || values[4].hi != 0x40000000UL)
        return 10;
    if (CALL(a, "convert", NULL, 0, 5) || count != 5 ||
        values[0].lo != 0x5f000001UL || values[0].hi != 0UL ||
        values[1].hi != 0x43f00000UL || values[1].lo != 0UL ||
        values[2].lo != 0x80000000UL || values[2].hi != 0UL ||
        values[3].lo != 0xffffffffUL || values[3].hi != 0xffffffffUL ||
        values[4].lo != 0UL || values[4].hi != 0UL)
        return 11;
    args[0].width = 64U; args[0].lo = 1UL; args[0].hi = 0x7ff00000UL;
    if (CALL(a, "trunc", args, 1, 1) != 9 || count != 0)
        return 12;
    args[0].lo = 0UL; args[0].hi = 0x43e00000UL;
    if (CALL(a, "trunc", args, 1, 1) != 5 || count != 0)
        return 13;
    args[0].hi = 0xc3e00000UL;
    if (CALL(a, "trunc", args, 1, 1) || count != 1 ||
        values[0].hi != 0x80000000UL || values[0].lo != 0UL)
        return 14;
    wasm_instance_destroy(a);
    wasm_instance_destroy(b);
    return 0;
}
]=])
run_ok(COMMAND "${ASSEMBLE}" "${dir}/state.wat" OUTPUT_FILE "${dir}/state.wasm")
run_ok(COMMAND "${WASM2C}" "${dir}/state.wasm" OUTPUT_FILE "${dir}/state.c")
run_ok(COMMAND "${WAT2C}" --module "${dir}/state.wat" OUTPUT_FILE "${dir}/state_wat.c")
run_ok(COMMAND "${CMAKE_COMMAND}" -E compare_files "${dir}/state.c" "${dir}/state_wat.c")
run_ok(COMMAND "${CC}" ${cflags} -std=c90 -pedantic-errors -Wall -Wextra -Werror
  "${dir}/state.c" "${dir}/state-driver.c" ${link_flags} -lm -o "${dir}/state-driver${EXE_SUFFIX}")
run_ok(COMMAND "${dir}/state-driver${EXE_SUFFIX}")

# A legal but uninstantiable module must still translate; bounds are checked at runtime.
file(WRITE "${dir}/bounds.wat" "(module (memory 0) (data (i32.const 0) \"x\"))\n")
file(WRITE "${dir}/bounds-driver.c" [=[
#include <stddef.h>
struct wasm_instance;
int wasm_instance_create(struct wasm_instance **, unsigned long);
int main(void)
{
    struct wasm_instance *instance;
    return wasm_instance_create(&instance, 1UL) != 6 || instance != NULL;
}
]=])
run_ok(COMMAND "${WAT2C}" --module "${dir}/bounds.wat" OUTPUT_FILE "${dir}/bounds.c")
run_ok(COMMAND "${CC}" ${cflags} -std=c90 -pedantic-errors -Wall -Wextra -Werror
  "${dir}/bounds.c" "${dir}/bounds-driver.c" ${link_flags} -o "${dir}/bounds-driver${EXE_SUFFIX}")
run_ok(COMMAND "${dir}/bounds-driver${EXE_SUFFIX}")

file(WRITE "${dir}/table.wat" [=[
(module
  (type $unary (func (param i32) (result i32)))
  (type $same (func (param i32) (result i32)))
  (func $host (export "host") (import "env\00x" "increment") (type $unary))
  (func $ping (import "env" "ping"))
  (func $other (type $same) local.get 0 i32.const 2 i32.mul)
  (func $wrong (result i64) i64.const 42)
  (table $table (export "table") 4 5 funcref)
  (elem (i32.const 0) func $host $other $wrong)
  (elem $passive func $other)
  (func $start call $ping)
  (start $start)
  (func (export "indirect") (param i32 i32) (result i32)
    local.get 0 local.get 1 call_indirect (type $unary))
  (func (export "bulk") (result i32 i32)
    i32.const 3 i32.const 0 i32.const 1 table.init $table $passive
    elem.drop $passive
    i32.const 2 i32.const 3 i32.const 1 table.copy $table $table
    i32.const 1 ref.null func i32.const 1 table.fill $table
    ref.func $other i32.const 1 table.grow $table
    table.size $table)
  (func (export "inspect") (param i32) (result i32)
    local.get 0 table.get $table ref.is_null)
  (func (export "clear") (param i32)
    local.get 0 ref.null func table.set $table))
]=])
file(WRITE "${dir}/table-driver.c" [=[
#include <stddef.h>
struct wasm_result { unsigned width; unsigned long lo, hi; };
struct wasm_instance;
typedef int (*wasm_host_callback)(void *, const struct wasm_result *, size_t,
                                  struct wasm_result *, size_t);
struct wasm_host_import {
    const char *module, *name;
    size_t module_size, name_size;
    wasm_host_callback callback;
    void *context;
};
int wasm_instance_create(struct wasm_instance **, unsigned long);
int wasm_instance_create_with_imports(struct wasm_instance **, const struct wasm_host_import *,
                                      size_t, unsigned long);
void wasm_instance_destroy(struct wasm_instance *);
int wasm_instance_invoke(struct wasm_instance *, const char *, size_t,
                         const struct wasm_result *, size_t, struct wasm_result *,
                         size_t, size_t *, unsigned long);
int wasm_instance_get_table(struct wasm_instance *, const char *, size_t,
                            unsigned long, struct wasm_result *);
int wasm_instance_set_table(struct wasm_instance *, const char *, size_t,
                            unsigned long, const struct wasm_result *);
#define CALL(name, args, nargs) \
    wasm_instance_invoke(a, name, sizeof(name) - 1U, args, nargs, values, 2, &count, 1000UL)

static int host(void *ctx, const struct wasm_result *args, size_t nargs,
                struct wasm_result *out, size_t nresults)
{
    int mode = *(const int *)ctx;
    if (nargs != 1 || nresults != 1 || args[0].width != 32U || mode == 2)
        return -1;
    out[0] = args[0];
    if (mode == 1)
        out[0].width = 64U;
    out[0].lo = (out[0].lo + 1UL) & 0xffffffffUL;
    return 0;
}

static int ping(void *ctx, const struct wasm_result *args, size_t nargs,
                struct wasm_result *out, size_t nresults)
{
    (void)args; (void)out;
    if (nargs != 0 || nresults != 0)
        return -1;
    ++*(int *)ctx;
    return 0;
}

int main(void)
{
    int mode = 0, starts = 0;
    struct wasm_host_import imports[] = {
        {"env\0x", "increment", 5, 9, host, NULL},
        {"env", "ping", 3, 4, ping, NULL},
        {"env\0x", "increment", 5, 9, host, NULL}
    };
    struct wasm_instance *a;
    struct wasm_result values[2], args[2] = {{32U, 42UL, 0UL}, {32U, 0UL, 0UL}};
    size_t count;
    imports[0].context = &mode;
    imports[1].context = &starts;
    if (wasm_instance_create(&a, 1000UL) != 15 || a != NULL ||
        wasm_instance_create_with_imports(&a, imports, 3, 1000UL) != 3 || a != NULL ||
        wasm_instance_create_with_imports(&a, imports, 2, 0UL) != 7 || a != NULL || starts != 0 ||
        wasm_instance_create_with_imports(&a, imports, 2, 2UL) || starts != 1)
        return 1;
    if (CALL("indirect", args, 2) || count != 1 || values[0].lo != 43UL)
        return 2;
    args[1].lo = 1UL;
    if (CALL("indirect", args, 2) || values[0].lo != 84UL)
        return 3;
    args[1].lo = 2UL;
    if (CALL("indirect", args, 2) != 13 || count != 0)
        return 4;
    args[1].lo = 3UL;
    if (CALL("indirect", args, 2) != 12 || count != 0)
        return 5;
    args[1].lo = 4UL;
    if (CALL("indirect", args, 2) != 11 || count != 0)
        return 6;
    if (CALL("bulk", NULL, 0) || count != 2 || values[0].lo != 4UL || values[1].lo != 5UL ||
        CALL("bulk", NULL, 0) != 11)
        return 7;
    if (CALL("indirect", args, 2) || values[0].lo != 84UL ||
        CALL("inspect", args + 1, 1) || values[0].lo != 0UL ||
        wasm_instance_get_table(a, "table", 5, 4UL, values) || values[0].lo != 3UL ||
        CALL("clear", args + 1, 1) || CALL("inspect", args + 1, 1) || values[0].lo != 1UL)
        return 8;
    values[0].width = 64U; values[0].lo = 1UL; values[0].hi = 0UL;
    if (wasm_instance_set_table(a, "table", 5, 4UL, values) ||
        CALL("indirect", args, 2) || values[0].lo != 43UL)
        return 9;
    if (wasm_instance_invoke(a, "host", 4, args, 1, values, 1, &count, 0UL) != 7 ||
        wasm_instance_invoke(a, "host", 4, args, 1, values, 1, &count, 1UL) || values[0].lo != 43UL)
        return 10;
    mode = 1;
    if (CALL("host", args, 1) != 14 || count != 0)
        return 11;
    mode = 2;
    if (CALL("indirect", args, 2) != 14 || count != 0)
        return 12;
    wasm_instance_destroy(a);
    return 0;
}
]=])
run_ok(COMMAND "${ASSEMBLE}" "${dir}/table.wat" OUTPUT_FILE "${dir}/table.wasm")
run_ok(COMMAND "${WASM2C}" "${dir}/table.wasm" OUTPUT_FILE "${dir}/table.c")
run_ok(COMMAND "${WAT2C}" --module "${dir}/table.wat" OUTPUT_FILE "${dir}/table_wat.c")
run_ok(COMMAND "${CMAKE_COMMAND}" -E compare_files "${dir}/table.c" "${dir}/table_wat.c")
run_ok(COMMAND "${CC}" ${cflags} -std=c90 -pedantic-errors -Wall -Wextra -Werror
  "${dir}/table.c" "${dir}/table-driver.c" ${link_flags} -o "${dir}/table-driver${EXE_SUFFIX}")
run_ok(COMMAND "${dir}/table-driver${EXE_SUFFIX}")

file(WRITE "${dir}/unsupported.wat" "(module (import \"env\" \"mem\" (memory 1)))\n")
execute_process(COMMAND "${WAT2C}" --module "${dir}/unsupported.wat"
  OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status)
if(status STREQUAL "0" OR NOT output STREQUAL "" OR NOT error MATCHES "non-function imports")
  message(FATAL_ERROR "module outside the C90 profile was not rejected (${status}): ${error}")
endif()
