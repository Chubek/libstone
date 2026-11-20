set(DOMVIRT_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(DOMVIRT_SRC_DIR "${PROJECT_SOURCE_DIR}/src")
set(DOMVIRT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/../generated")

foreach(MACHINE IN ITEMS x86_64 aarch64 riscv64)
    add_test(NAME boot_${MACHINE} COMMAND ${Python_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/e2e/boot_system.py" $<TARGET_FILE:domvirt_cli> ${MACHINE} software)
    set_tests_properties(boot_${MACHINE} PROPERTIES TIMEOUT 45)
    add_test(NAME ipc_${MACHINE} COMMAND ${Python_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/e2e/ipc_testing.py" $<TARGET_FILE:domvirt_cli> ${MACHINE} software)
    set_tests_properties(ipc_${MACHINE} PROPERTIES TIMEOUT 90)
endforeach()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_test(NAME bios_native_kvm COMMAND ${Python_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/e2e/bios_boot.py" $<TARGET_FILE:domvirt_cli>)
    set_tests_properties(bios_native_kvm PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
    add_test(NAME boot_native_kvm COMMAND ${Python_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/e2e/boot_system.py" $<TARGET_FILE:domvirt_cli> x86_64 kvm)
    set_tests_properties(boot_native_kvm PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
    add_test(NAME ipc_native_kvm COMMAND ${Python_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/e2e/ipc_testing.py" $<TARGET_FILE:domvirt_cli> x86_64 kvm)
    set_tests_properties(ipc_native_kvm PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
add_test(NAME machine_contract COMMAND ${Python_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/e2e/machine_contract.py" $<TARGET_FILE:domvirt_cli>
    "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}")
set_tests_properties(machine_contract PROPERTIES TIMEOUT 60)
add_test(NAME library_boot COMMAND ${Python_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/e2e/library_boot.py" $<TARGET_FILE:domvirt>)
set_tests_properties(library_boot PROPERTIES TIMEOUT 30)
add_test(NAME control_socket COMMAND ${Python_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/e2e/control_socket.py" $<TARGET_FILE:domvirt_cli>)
set_tests_properties(control_socket PROPERTIES TIMEOUT 15)
add_executable(imgui_display_test unit/test_imgui_display.cpp "${PROJECT_SOURCE_DIR}/src/cli/boot_display.cpp")
target_include_directories(imgui_display_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(imgui_display_test PRIVATE archvirt_imgui Threads::Threads)
domvirt_set_compiler_flags(imgui_display_test)
add_test(NAME imgui_display COMMAND imgui_display_test)
set_tests_properties(imgui_display PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 10)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found; unit tests will be skipped")
    return()
endif()

set(UNIT_TEST_SOURCES
    ${DOMVIRT_TEST_DIR}/unit/test_memory_manager.cpp
    ${DOMVIRT_TEST_DIR}/unit/test_virtqueue.cpp
    ${DOMVIRT_TEST_DIR}/unit/test_lazy_register.cpp
    ${DOMVIRT_TEST_DIR}/unit/test_boot_display.cpp
    ${DOMVIRT_TEST_DIR}/unit/test_virtual_cpu.cpp
    ${DOMVIRT_TEST_DIR}/unit/test_machine.cpp
    ${DOMVIRT_TEST_DIR}/unit/test_riscv64.cpp
)

add_executable(domvirt_unit_tests ${UNIT_TEST_SOURCES})
target_include_directories(domvirt_unit_tests PRIVATE
    ${DOMVIRT_SRC_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${DOMVIRT_GEN_DIR}
    ${PROJECT_SOURCE_DIR}/domios/src
)
target_link_libraries(domvirt_unit_tests PRIVATE
    domvirt_core
    GTest::GTest
    GTest::Main
)
domvirt_set_compiler_flags(domvirt_unit_tests)

add_test(NAME domvirt_unit_tests COMMAND domvirt_unit_tests)

set(BRIDGE_TEST_SOURCES
    ${DOMVIRT_TEST_DIR}/bridge/test_bridge.cpp
    ${DOMVIRT_TEST_DIR}/bridge/test_shared_memory.cpp
    ${DOMVIRT_TEST_DIR}/bridge/test_dispatcher.cpp
)

add_executable(domvirt_bridge_tests ${BRIDGE_TEST_SOURCES})
target_include_directories(domvirt_bridge_tests PRIVATE
    ${DOMVIRT_SRC_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${DOMVIRT_GEN_DIR}
)
target_link_libraries(domvirt_bridge_tests PRIVATE
    domvirt_core
    GTest::GTest
    GTest::Main
)
domvirt_set_compiler_flags(domvirt_bridge_tests)

add_test(NAME domvirt_bridge_tests COMMAND domvirt_bridge_tests)

set(E2E_TEST_SOURCES
    ${DOMVIRT_TEST_DIR}/e2e/test_boot.cpp
    ${DOMVIRT_TEST_DIR}/e2e/test_virtio.cpp
)

add_executable(domvirt_e2e_tests ${E2E_TEST_SOURCES})
target_include_directories(domvirt_e2e_tests PRIVATE
    ${DOMVIRT_SRC_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${DOMVIRT_GEN_DIR}
)
target_link_libraries(domvirt_e2e_tests PRIVATE
    domvirt_core
    GTest::GTest
    GTest::Main
)
domvirt_set_compiler_flags(domvirt_e2e_tests)

add_test(NAME domvirt_e2e_tests COMMAND domvirt_e2e_tests)

add_subdirectory(domvirt-test-suite)

if(DOMVIRT_BUILD_BENCHMARKS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(BENCHMARK_SOURCES
        ${DOMVIRT_TEST_DIR}/benchmarks/bench_vcpu.cpp
        ${DOMVIRT_TEST_DIR}/benchmarks/bench_bridge.cpp
        ${DOMVIRT_TEST_DIR}/benchmarks/bench_virtio.cpp
    )

    add_executable(domvirt_benchmarks ${BENCHMARK_SOURCES})
    target_include_directories(domvirt_benchmarks PRIVATE
        ${DOMVIRT_SRC_DIR}
        ${PROJECT_SOURCE_DIR}/include
        ${DOMVIRT_GEN_DIR}
    )
    target_link_libraries(domvirt_benchmarks PRIVATE
        domvirt_core
    )
    domvirt_set_compiler_flags(domvirt_benchmarks)
    domvirt_set_release_flags(domvirt_benchmarks)
endif()
