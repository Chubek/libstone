function(ok)
  execute_process(COMMAND ${ARGV} WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "${ARGV} failed (${status})\n${out}\n${err}")
  endif()
endfunction()

function(fail code pattern)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK}"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT status STREQUAL "${code}" OR NOT err MATCHES "${pattern}")
    message(FATAL_ERROR "${ARGN}: expected ${code} and '${pattern}', got ${status}\n${out}\n${err}")
  endif()
endfunction()

file(MAKE_DIRECTORY "${WORK}/api" "${WORK}/cli")
file(COPY "${EXAMPLES}/Minimal.am8" "${EXAMPLES}/Generator.am8" DESTINATION "${WORK}/api")
file(COPY "${EXAMPLES}/Generator.am8" DESTINATION "${WORK}/cli")
ok("${AMALGAM8}" --help)
fail(2 "unknown option" "${AMALGAM8}" --wrong)

file(REMOVE "${WORK}/cli/Generator.hpp")
ok("${AMALGAM8}" --cli cli/Generator.am8)
ok("${CXX}" -std=c++11 -Wall -Wextra -Werror -pedantic cli/Generator.cpp -o cli/tool)
ok("${WORK}/cli/tool" x)

ok("${AMALGAM8}" api/Minimal.am8)
file(READ "${WORK}/api/Minimal.hpp" implicit)
ok("${AMALGAM8}" --api api/Minimal.am8)
file(READ "${WORK}/api/Minimal.hpp" explicit)
if(NOT implicit STREQUAL explicit OR implicit MATCHES "${WORK}")
  message(FATAL_ERROR "API modes differ or leak the workspace path")
endif()
file(WRITE "${WORK}/api/peer.c" [=[
#include "Minimal.hpp"
int peer(void) { return minimal_answer(1); }
]=])
file(WRITE "${WORK}/api/main.c" [=[
#include "Minimal.hpp"
#include "Minimal.hpp"
int peer(void);
int main(void) { return peer() != 41 || minimal_answer(2) != 42; }
]=])
ok("${CC}" -std=c11 -Wall -Wextra -Werror -pedantic api/peer.c api/main.c -o api/c-consumer)
ok("${WORK}/api/c-consumer")
ok("${CXX}" -x c++ -std=c++11 -Wall -Wextra -Werror -pedantic api/peer.c api/main.c -o api/cpp-consumer)
ok("${WORK}/api/cpp-consumer")

file(WRITE "${WORK}/api/Typed.am8" [=[
(library Typed
  (include limits.h)
  (def LEGACY_LIMIT INT_MAX)
  (fn bool positive ((uint32_t value)) (return (> value 0))))
]=])
ok("${AMALGAM8}" --deps api/Typed.am8)
file(WRITE "${WORK}/api/typed.c" [=[
#include "Typed.hpp"
int main(void) { return LEGACY_LIMIT != INT_MAX || !positive(42); }
]=])
ok("${CC}" -std=c11 -Wall -Wextra -Werror -pedantic api/typed.c -o api/typed)
ok("${WORK}/api/typed")

file(WRITE "${WORK}/api/Minimal.am8" "(library Minimal\n  (fn int missing ()))")
fail(1 "Minimal\\.am8:2:[0-9]+" "${AMALGAM8}" api/Minimal.am8)
file(READ "${WORK}/api/Minimal.hpp" after_failure)
if(NOT after_failure STREQUAL implicit)
  message(FATAL_ERROR "failed compile changed a previous valid output")
endif()

file(WRITE "${WORK}/cli/A.am8" "(library A (import \"B.am8\"))")
file(WRITE "${WORK}/cli/B.am8" "(module B (import \"A.am8\"))")
fail(1 "[Cc]ycle" "${AMALGAM8}" cli/A.am8)
file(WRITE "${WORK}/cli/A.am8" "(module A (export shared) (fn int shared () (block (return 1))))")
file(WRITE "${WORK}/cli/B.am8" "(module B (export shared) (fn int shared () (block (return 2))))")
file(WRITE "${WORK}/cli/Bad.am8" "(library Bad (import \"A.am8\") (import \"B.am8\") (fn int run () (block (return (call shared)))))")
fail(1 "[Aa]mbigu" "${AMALGAM8}" cli/Bad.am8)
file(WRITE "${WORK}/cli/Qualified.am8" "(library Qualified (import \"A.am8\" A) (import \"B.am8\" B) (fn int combined () (block (return (+ (call A.shared) (call B.shared))))) (cli \"return combined() != 3;\"))")
ok("${AMALGAM8}" --cli cli/Qualified.am8)
ok("${CXX}" -std=c++11 -Wall -Wextra -Werror -pedantic cli/Qualified.cpp -o cli/qualified)
ok("${WORK}/cli/qualified")
file(WRITE "${WORK}/cli/A.am8" "(module A (export shared) (extern int shared ()))")
file(WRITE "${WORK}/cli/B.am8" "(module B (export shared) (extern int shared ()))")
file(WRITE "${WORK}/cli/Bad.am8" "(library Bad (import \"A.am8\" A) (import \"B.am8\" B))")
fail(1 "[Cc]onflict|[Dd]uplicate" "${AMALGAM8}" cli/Bad.am8)
file(WRITE "${WORK}/cli/Bad.am8" "(library Bad (stage \"not a declaration\"))")
fail(1 "Bad\\.am8:1:" "${AMALGAM8}" cli/Bad.am8)
file(WRITE "${WORK}/cli/Bad.am8" "(library Bad (native unavailable))")
fail(1 "unavailable" "${AMALGAM8}" cli/Bad.am8)
file(WRITE "${WORK}/cli/Bad.am8" "(library Bad)")
fail(1 "cli" "${AMALGAM8}" --cli cli/Bad.am8)

# A directory is a deterministic rename failure even when running as root.
file(WRITE "${WORK}/cli/Blocked.am8" "(library Blocked)")
file(MAKE_DIRECTORY "${WORK}/cli/Blocked.hpp")
file(WRITE "${WORK}/cli/Blocked.hpp/survivor" "keep")
fail(1 "publish output" "${AMALGAM8}" cli/Blocked.am8)
file(GLOB temps "${WORK}/cli/Blocked.hpp.*")
if(temps OR NOT EXISTS "${WORK}/cli/Blocked.hpp/survivor")
  message(FATAL_ERROR "failed publication leaked a temporary file or changed destination")
endif()
