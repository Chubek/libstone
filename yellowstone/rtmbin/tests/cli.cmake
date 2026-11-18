file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${FIXTURES}" "${WORK}" COMMAND_ERROR_IS_FATAL ANY)

function(check expected regex)
  execute_process(COMMAND "${TOOL}" ${ARGN} RESULT_VARIABLE code
                  OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT "${code}" STREQUAL "${expected}" OR NOT "${out}${err}" MATCHES "${regex}")
    message(FATAL_ERROR "rtmbin ${ARGN}: expected ${expected} / ${regex}, got ${code}\n${out}${err}")
  endif()
  if(NOT "${expected}" STREQUAL "0" AND NOT "${out}" STREQUAL "")
    message(FATAL_ERROR "rtmbin ${ARGN}: unexpected partial output\n${out}")
  endif()
endfunction()

check(0 "format=ELF arch=x86-64 bits=64 preferred_base=0x400000" layout "${WORK}/image.elf")
check(0 "status=attributed linked_pc=0x402003 range=1 file_offset=0x203"
      attribute "${WORK}/image.elf" 0x40002003 --runtime-base 0x40000000)
check(0 "status=outside_ranges" attribute "${WORK}/image.exe" 0x140001010)
check(0 "status=attributed linked_pc=0x140001000 range=0 file_offset=0x200"
      attribute "${WORK}/image.exe" 0x140001000)
check(0 "status=attributed linked_pc=0x100000000 range=1 file_offset=0x0"
      attribute "${WORK}/image.macho" 0x100000000)
check(0 "name=\"__D\\\\x1bTA\"" layout "${WORK}/image.macho")
check(0 "status=not_file_backed linked_pc=0x100000300 range=1"
      attribute "${WORK}/image.macho" 0x100000300)
check(1 "regular file" layout "${WORK}")
check(2 "Usage:" attribute "${WORK}/image.elf" 0x10000000000000000)

file(WRITE "${WORK}/samples.txt"
  "# synthetic trace\r\n0x402000 2\r\n0x402003\r\n0x402004 4\r\n0x402008 8\r\n0x400001 16")
check(0 "total=31 attributed=3 outside_ranges=8 non_executable=16 not_file_backed=4 address_out_of_range=0"
      profile "${WORK}/image.elf" "${WORK}/samples.txt")
file(WRITE "${WORK}/samples-bad.txt" "0x402000\n0x402001 0\n")
check(1 "line 2:" profile "${WORK}/image.elf" "${WORK}/samples-bad.txt")
file(WRITE "${WORK}/samples-overflow.txt" "0x402000 18446744073709551615\n0x402001\n")
check(1 "combined sample weight" profile "${WORK}/image.elf" "${WORK}/samples-overflow.txt")
string(REPEAT "x" 4097 line)
file(WRITE "${WORK}/samples-bounds.txt" "#${line}\n")
check(1 "4096 bytes" profile "${WORK}/image.elf" "${WORK}/samples-bounds.txt")

file(WRITE "${WORK}/points.txt" "# caller-selected spans\nstart 0x70001000 2\nlast 0x7000100f")
file(SHA256 "${WORK}/image.exe" before_plan)
check(0 "point=\"start\" range=0 linked_pc=0x140001000 file_offset=0x200 size=0x2\npoint=\"last\" range=0 linked_pc=0x14000100f file_offset=0x20f size=0x1"
      plan "${WORK}/image.exe" "${WORK}/points.txt" --runtime-base 0x70000000)
file(SHA256 "${WORK}/image.exe" after_plan)
if(NOT "${before_plan}" STREQUAL "${after_plan}")
  message(FATAL_ERROR "rtmbin plan changed its input")
endif()
file(WRITE "${WORK}/points-bad.txt" "point.one 0x100000000\npoint.two 0x1000002ff 2\n")
check(1 "span crosses a file-backed range end: point.two"
      plan "${WORK}/image.macho" "${WORK}/points-bad.txt")

file(WRITE "${WORK}/graph.txt" "entry 0\nblock 0 1\nblock 1 0\nblock 2 # unreachable\n")
check(0 "reachable: 0 1\nunreachable: 2\ncycle: 0 1" graph "${WORK}/graph.txt")
file(WRITE "${WORK}/graph-bad.txt" "entry 0\nblock 0 8\n")
check(1 "absent block" graph "${WORK}/graph-bad.txt")
