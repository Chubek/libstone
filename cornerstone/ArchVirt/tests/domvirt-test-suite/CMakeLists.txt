set(DOMVIRT_TEST_SUITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(DOMVIRT_TEST_SUITE_SOURCES
    ${DOMVIRT_TEST_SUITE_DIR}/boot_memory_tests.cpp
    ${DOMVIRT_TEST_SUITE_DIR}/ipc_bridge_tests.cpp
    ${DOMVIRT_TEST_SUITE_DIR}/ipc_queue_tests.cpp
    ${DOMVIRT_TEST_SUITE_DIR}/shared_memory_tests.cpp
    ${DOMVIRT_TEST_SUITE_DIR}/virtqueue_acpi_tests.cpp
)

add_executable(domvirt_test_suite ${DOMVIRT_TEST_SUITE_SOURCES})
target_include_directories(domvirt_test_suite PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/domios/src
    ${PROJECT_SOURCE_DIR}/domios/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(domvirt_test_suite PRIVATE
    domvirt_core
    DOMWeave::domios
    GTest::GTest
    GTest::Main
)
domvirt_set_compiler_flags(domvirt_test_suite)

add_test(NAME domvirt_test_suite COMMAND domvirt_test_suite)
