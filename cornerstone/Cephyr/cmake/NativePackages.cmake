# Build trusted host backend modules and target-side CRT/libc exactly once.
set(CEPHYR_PREBUILT_RUNTIME_ROOT "" CACHE PATH "Trusted prebuilt target crt0.o and libc.a packages")
find_program(CEPHYR_PACKAGE_AR NAMES ar REQUIRED)
set(cephyr_packages "${CMAKE_CURRENT_BINARY_DIR}/native-packages")
file(GLOB_RECURSE cephyr_generator_inputs CONFIGURE_DEPENDS
  "${cephyr_archon}/archon/*.py" "${cephyr_archon}/archon/*.c" "${cephyr_archon}/archon/*.h"
  "${cephyr_archon}/archon/*.c.in" "${cephyr_archon}/archon/*.pl" "${cephyr_archon}/stdspec/*.arch")
file(GLOB_RECURSE cephyr_runtime_inputs CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/src/*" "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/include/*")
function(cephyr_package arch abi spec triple)
  # Trusted target bootstrap compilers build freestanding objects only; the
  # linked program uses the Cephyr archive, never the compiler's libraries.
  set(cc_names "${triple}-gcc")
  foreach(version RANGE 16 12 -1)
    list(APPEND cc_names "${triple}-gcc-${version}")
  endforeach()
  if(triple MATCHES "^${CMAKE_HOST_SYSTEM_PROCESSOR}-linux-" AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND cc_names gcc)
  endif()
  set(dir "${cephyr_packages}/${arch}")
  set(dest "${cephyr_data_dir}/native-packages/${arch}")
  set(sources Backend.c Legalizer.c MIRText.c NativeVerify.c NativeABIData.c ${ARGN})
  set(paths ${sources})
  list(TRANSFORM paths PREPEND "${dir}/")
  add_custom_command(OUTPUT ${paths} "${dir}/provider.archdb" "${dir}/sources.sha256"
    COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_backend.py"
      "${cephyr_archon}" "${spec}" "${arch}" "${abi}" "${dir}" ${sources}
    DEPENDS cmake/package_backend.py ${cephyr_generator_inputs}
      "${cephyr_repo}/cornerstone/ASDL/scripts/asdl.pl" "${cephyr_repo}/cornerstone/Aurocks/scripts/aurocks.pl" "${cephyr_repo}/LICENSE"
    VERBATIM)
  add_library(cephyr_backend_${arch} MODULE ${paths})
  set_target_properties(cephyr_backend_${arch} PROPERTIES C_STANDARD 99
    OUTPUT_NAME backend LIBRARY_OUTPUT_DIRECTORY "${dir}")
  if(CEPHYR_PREBUILT_RUNTIME_ROOT)
    set(input "${CEPHYR_PREBUILT_RUNTIME_ROOT}/${arch}")
    add_custom_command(OUTPUT "${dir}/crt0.o" "${dir}/libc.a"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${input}/crt0.o" "${input}/libc.a" "${dir}"
      DEPENDS "${input}/crt0.o" "${input}/libc.a" VERBATIM)
  else()
    find_program(CEPHYR_PACKAGE_CC_${arch} NAMES ${cc_names})
    if(NOT CEPHYR_PACKAGE_CC_${arch})
      message(FATAL_ERROR "Packaging ${triple} libc requires ${triple}-gcc. Set CEPHYR_PACKAGE_CC_${arch} or supply CEPHYR_PREBUILT_RUNTIME_ROOT.")
    endif()
    add_custom_command(OUTPUT "${dir}/crt0.o" "${dir}/libc.a"
      COMMAND "${CMAKE_COMMAND}" -E env --unset=CEPHYR_LIBC_TARGET_FLAGS "CEPHYR_LIBC_TARGET_OPTION="
        "CEPHYR_LIBC_CFLAGS=-fno-pic -fno-pie" "CC=${CEPHYR_PACKAGE_CC_${arch}}" "AR=${CEPHYR_PACKAGE_AR}"
        "CEPHYR_LIBC_TARGET=${triple}" sh "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/build.sh" "${dir}"
      DEPENDS stdlib/build.sh ${cephyr_runtime_inputs} "${CEPHYR_PACKAGE_CC_${arch}}" "${CEPHYR_PACKAGE_AR}" VERBATIM)
  endif()
  add_custom_target(cephyr_runtime_${arch} DEPENDS "${dir}/crt0.o" "${dir}/libc.a")
  add_dependencies(cephyr_compiler cephyr_backend_${arch} cephyr_runtime_${arch})
  install(TARGETS cephyr_backend_${arch} LIBRARY DESTINATION "${dest}")
  install(FILES "${dir}/provider.archdb" "${dir}/sources.sha256" "${dir}/crt0.o" "${dir}/libc.a" DESTINATION "${dest}")
endfunction()
cephyr_package(AMD64 SysVAMD64 amd64/root.arch x86_64-linux-gnu ScalarGAS.c AMD64GAS.c)
cephyr_package(AArch64 AAPCS64 aarch64/root.arch aarch64-linux-gnu ScalarGAS.c AArch64GAS.c)
cephyr_package(RISCV64IM LP64 riscv/rv64im.arch riscv64-linux-gnu ABI.c CFGLowering.c
  FrameLowering.c IntegerConversion.c MachineIR.c NativeEmitter.c NativeEncoding.c NativeFrameVerify.c
  NativeHomes.c NativeISel.c NativeLayout.c NativeRegalloc.c NativeSchedule.c NativeEncodingData.c)
target_compile_definitions(cephyr_compiler PRIVATE CEPHYR_BUILD_PACKAGES="${cephyr_packages}")
