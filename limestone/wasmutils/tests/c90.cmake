set(dir "${BINARY}/c90")
file(MAKE_DIRECTORY "${dir}")
separate_arguments(cflags NATIVE_COMMAND "${CFLAGS}")
separate_arguments(link_flags NATIVE_COMMAND "${LINK_FLAGS}")

function(run)
  execute_process(COMMAND ${ARGV} WORKING_DIRECTORY "${dir}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "${ARGV} failed (${status}):\n${output}${errors}")
  endif()
endfunction()

function(translate tool input output)
  execute_process(COMMAND "${tool}" "${input}" WORKING_DIRECTORY "${dir}"
    RESULT_VARIABLE status OUTPUT_FILE "${dir}/${output}" ERROR_VARIABLE errors)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "${tool} ${input} failed (${status}):\n${errors}")
  endif()
endfunction()

function(reject tool input)
  execute_process(COMMAND "${tool}" "${input}" WORKING_DIRECTORY "${dir}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT status MATCHES "^[1-9][0-9]*$" OR NOT output STREQUAL "" OR errors STREQUAL "")
    message(FATAL_ERROR "${tool} did not cleanly reject ${input} (${status}):\n${output}${errors}")
  endif()
endfunction()

function(compile_and_run driver name)
  run("${CC}" ${cflags} -std=c90 -pedantic-errors -Wall -Wextra -Werror "${driver}"
    ${link_flags} -o "${name}${EXE_SUFFIX}")
  run("${dir}/${name}${EXE_SUFFIX}")
endfunction()

run("${FIXTURES}" --fixtures "${dir}")
translate("${WAT2C}" fixture.wat wat.c)
translate("${WASM2C}" fixture.ops wasm.c)
run("${CMAKE_COMMAND}" -E compare_files wat.c wasm.c)
execute_process(COMMAND "${WASM2C}" - WORKING_DIRECTORY "${dir}" INPUT_FILE "${dir}/fixture.ops"
  RESULT_VARIABLE status OUTPUT_FILE "${dir}/stdin.c" ERROR_VARIABLE errors)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "wasm2c stdin failed (${status}):\n${errors}")
endif()
run("${CMAKE_COMMAND}" -E compare_files wasm.c stdin.c)

file(WRITE "${dir}/trap.wat" "i32.const 1 unreachable i64.add drop\n")
translate("${WAT2C}" trap.wat trap.c)
file(WRITE "${dir}/empty.wat" "nop\n")
translate("${WAT2C}" empty.wat empty.c)
file(WRITE "${dir}/wasm-driver.c" [=[
#include "wasm.c"

static const struct wasm_result expected[] = {
#include "expected.inc"
};

int main(void)
{
    struct wasm_result out[sizeof expected / sizeof expected[0]];
    size_t count, i;
    out[0].width = 0;
    out[0].lo = 123;
    out[0].hi = 456;
    if (wasm_run(out, 0, &count) != 2 || count != sizeof expected / sizeof expected[0] ||
        out[0].width != 0 || out[0].lo != 123 || out[0].hi != 456)
        return 1;
    if (wasm_run(0, count, 0) != 2 || wasm_run(out, count, 0) != 0)
        return 2;
    for (i = 0; i < count; ++i) {
        if (out[i].width != expected[i].width ||
            out[i].lo != expected[i].lo || out[i].hi != expected[i].hi)
            return 3;
    }
    return 0;
}
]=])
file(WRITE "${dir}/trap-driver.c" [=[
#include "trap.c"

int main(void)
{
    struct wasm_result out = {32, 123UL, 456UL};
    size_t count = 42;
    return wasm_run(&out, 1, &count) != 1 || count != 0 ||
           out.width != 32 || out.lo != 123UL || out.hi != 456UL;
}
]=])
file(WRITE "${dir}/empty-driver.c" [=[
#include "empty.c"

int main(void)
{
    size_t count = 42;
    return wasm_run(0, 0, &count) != 0 || count != 0;
}
]=])
compile_and_run(wasm-driver.c wasm-driver)
compile_and_run(trap-driver.c trap-driver)
compile_and_run(empty-driver.c empty-driver)

file(WRITE "${dir}/invalid.wat" "unreachable i64.const 0 i32.eqz\n")
reject("${WAT2C}" invalid.wat)
file(WRITE "${dir}/invalid.ops" "A")
reject("${WASM2C}" invalid.ops)
file(WRITE "${dir}/invalid.wit" "interface fixtures { test: func(arg: list<>); }\n")
reject("${WIT2C}" invalid.wit)

file(WRITE "${dir}/fixtures.wit" [=[
package tests:sample@0.1.0;

interface fixtures {
    increment: func(%type: u32) -> u32;
    wide-value: func(arg: u64) -> s64;
    finished: func();
}
]=])
translate("${WIT2C}" fixtures.wit bindings.h)
file(WRITE "${dir}/wit-driver.c" [=[
#include "bindings.h"
#include "bindings.h"

static int finished;

wit_host_u32 wit_tests_W_sample_W_v0v1v0_W_fixtures_W_increment(wit_host_u32 value)
{
    return value + 1U;
}

wit_host_s64 wit_tests_W_sample_W_v0v1v0_W_fixtures_W_wide_value(wit_host_u64 value)
{
    wit_host_s64 result;
    result.high = value.high;
    result.low = value.low;
    return result;
}

void wit_tests_W_sample_W_v0v1v0_W_fixtures_W_finished(void)
{
    finished = 1;
}

int main(void)
{
    wit_host_u64 input = {0xffffffffUL, 0xfffffffeUL};
    wit_host_s64 result = wit_tests_W_sample_W_v0v1v0_W_fixtures_W_wide_value(input);
    wit_tests_W_sample_W_v0v1v0_W_fixtures_W_finished();
    return !finished || result.high != input.high || result.low != input.low ||
           wit_tests_W_sample_W_v0v1v0_W_fixtures_W_increment(41U) != 42U;
}
]=])
compile_and_run(wit-driver.c wit-driver)
