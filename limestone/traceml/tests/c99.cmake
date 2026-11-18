set(out "${BINARY}/traceml-c99-test")
file(MAKE_DIRECTORY "${out}")

function(checked)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE status OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "${ARGV} failed (${status}):\n${stdout}${stderr}")
  endif()
endfunction()

function(emit name code)
  file(WRITE "${out}/${name}.trace" "${code}\n")
  checked("${CLI}" --emit-c --module "${name}" -o "${out}/${name}.c" "${out}/${name}.trace")
endfunction()

# The first form's value is discarded, and external arguments apply only to
# the last form. The unused argument and false branch each contain an overflow.
emit(calc "17
(lambda x (if (lt x 0) (neg x)
  ((lambda f (f x ((lambda ignore 2) (add 9223372036854775807 1)))) add)))")
emit(shadow "((lambda add (begin add ((lambda add (add 20 22)) (lambda x (lambda y (sub x (neg y))))))) 5)")
emit(closure "(((lambda x (lambda y (add x y))) 10) 32)")
emit(twice "((lambda x (add x x)) (add 20 1))")
emit(diverge "((lambda x (x x)) (lambda x (x x)))")
emit(overflow "; bytes before error
  (mul -1 -9223372036854775808)")
emit(earlier "(add 9223372036854775807 1)
(lambda x x)")
emit(constant "-9223372036854775808")
emit(arity "((lambda f (f 1)) add)")
emit(type_error "(if (lambda x x) 1 0)")
emit(left_first "(add (mul -1 -9223372036854775808) (if (lambda x x) 1 0))")
emit(callable "(lambda x x)")

checked("${CLI}" --module calc -o "${out}/repeat.c" "${out}/calc.trace")
checked("${CMAKE_COMMAND}" -E compare_files "${out}/calc.c" "${out}/repeat.c")

file(WRITE "${out}/invalid.trace" "foo\n")
file(WRITE "${out}/untouched.c" "sentinel")
execute_process(COMMAND "${CLI}" -o "${out}/untouched.c" "${out}/invalid.trace"
  RESULT_VARIABLE status ERROR_VARIABLE diagnostic)
file(READ "${out}/untouched.c" content)
if(status STREQUAL "0" OR NOT diagnostic MATCHES "unbound symbol" OR NOT content STREQUAL "sentinel")
  message(FATAL_ERROR "Invalid input did not fail without modifying the output: ${diagnostic}")
endif()
execute_process(COMMAND "${CLI}" --module "not-valid" "${out}/calc.trace"
  RESULT_VARIABLE status ERROR_VARIABLE diagnostic)
if(status STREQUAL "0" OR NOT diagnostic MATCHES "ASCII module identifier")
  message(FATAL_ERROR "Invalid module name did not fail: ${diagnostic}")
endif()

separate_arguments(flags NATIVE_COMMAND "${CFLAGS}")
separate_arguments(link_flags NATIVE_COMMAND "${LINK_FLAGS}")
set(strict)
if(CC_ID MATCHES "GNU|Clang")
  set(strict -std=c99 -pedantic-errors -Wall -Wextra -Werror)
endif()
checked("${CC}" ${flags} ${strict} "-I${INCLUDE_DIR}"
  "${SOURCE}/tests/c99_driver.c" "${out}/calc.c" "${out}/shadow.c" "${out}/closure.c" "${out}/twice.c"
  "${out}/diverge.c" "${out}/overflow.c" "${out}/constant.c" "${out}/arity.c"
  "${out}/type_error.c" "${out}/left_first.c" "${out}/callable.c" "${out}/earlier.c"
  "${RUNTIME}" ${link_flags} -o "${out}/run")
checked("${out}/run")
