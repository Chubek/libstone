find_program(NASM_EXECUTABLE nasm REQUIRED)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/archvirt-bios.bin"
    COMMAND "${NASM_EXECUTABLE}" -f bin "${CMAKE_CURRENT_SOURCE_DIR}/bios.asm"
        -o "${CMAKE_CURRENT_BINARY_DIR}/archvirt-bios.bin"
    DEPENDS bios.asm VERBATIM)
add_custom_target(pc_bios ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/archvirt-bios.bin")
