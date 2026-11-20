set(DOMIX_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
find_program(DOMIX_CLANG NAMES clang REQUIRED)
find_program(DOMIX_LLD NAMES ld.lld REQUIRED)

add_executable(domix_mkimg tools/mkimg/main.cpp)
target_link_libraries(domix_mkimg PRIVATE tomlplusplus)
domvirt_set_compiler_flags(domix_mkimg)

# These Linux-host utilities are kept as development tools, never mislabeled as
# executable Domix userland. The bundled system uses a resident guest shell.
add_executable(domixbox EXCLUDE_FROM_ALL tools/domixbox.c)
add_executable(domix_filebridge EXCLUDE_FROM_ALL tools/domix_filebridge.c)

file(GLOB_RECURSE DOMIX_ROOTFS_INPUTS CONFIGURE_DEPENDS "${DOMIX_DIR}/assets/rootfs/*")
set(DOMIX_IMAGES)
foreach(ARCH IN ITEMS x86_64 aarch64 riscv64)
    set(OUT "${CMAKE_CURRENT_BINARY_DIR}/${ARCH}")
    file(MAKE_DIRECTORY "${OUT}")
    if(ARCH STREQUAL "x86_64")
        set(ARCH_FLAGS --target=x86_64-none-elf -mno-red-zone -mno-sse -mno-sse2)
    elseif(ARCH STREQUAL "riscv64")
        set(ARCH_FLAGS --target=riscv64-none-elf -march=rv64im_zifencei -mabi=lp64 -mcmodel=medany -mno-relax -msmall-data-limit=0)
    else()
        set(ARCH_FLAGS --target=aarch64-none-elf -mgeneral-regs-only -mno-outline-atomics)
    endif()
    add_custom_command(OUTPUT "${OUT}/kernel"
        COMMAND "${DOMIX_CLANG}" ${ARCH_FLAGS} -fuse-ld=lld -O1
            -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie
            -fno-vectorize -fno-slp-vectorize -fno-unwind-tables -fno-asynchronous-unwind-tables
            -nostdlib -static -Wl,--build-id=none -Wl,-T,${DOMIX_DIR}/portable.ld
            -I${PROJECT_SOURCE_DIR}/include "${DOMIX_DIR}/start.S" "${DOMIX_DIR}/kernel.c"
            -o "${OUT}/kernel"
        DEPENDS start.S kernel.c portable.ld "${PROJECT_SOURCE_DIR}/include/archvirt_boot.h"
        VERBATIM)
    configure_file(assets/manifest.toml "${OUT}/manifest.toml" @ONLY)
    add_custom_command(OUTPUT "${OUT}/domix.img"
        COMMAND $<TARGET_FILE:domix_mkimg> --kernel "${OUT}/kernel"
            --rootfs "${DOMIX_DIR}/assets/rootfs" --manifest "${OUT}/manifest.toml"
            --output "${OUT}/domix.img"
        DEPENDS domix_mkimg "${OUT}/kernel" "${OUT}/manifest.toml" ${DOMIX_ROOTFS_INPUTS}
        VERBATIM)
    list(APPEND DOMIX_IMAGES "${OUT}/domix.img")
endforeach()
add_custom_target(domix-image ALL DEPENDS ${DOMIX_IMAGES})
# Compatibility paths for existing clients.
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/domix.img" "${CMAKE_CURRENT_BINARY_DIR}/kernel"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/x86_64/domix.img" "${CMAKE_CURRENT_BINARY_DIR}/domix.img"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/x86_64/kernel" "${CMAKE_CURRENT_BINARY_DIR}/kernel"
    DEPENDS domix-image "${CMAKE_CURRENT_BINARY_DIR}/x86_64/domix.img" VERBATIM)
add_custom_target(domix_compat ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/domix.img" "${CMAKE_CURRENT_BINARY_DIR}/kernel")
