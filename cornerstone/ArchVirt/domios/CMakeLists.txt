set(DOMIOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(DOMIOS_SRC_DIR "${DOMIOS_DIR}/src")
set(DOMIOS_INC_DIR "${DOMIOS_DIR}/include")
set(DOMIOS_TOOLS_DIR "${DOMIOS_DIR}/tools")
set(DOMIOS_TEST_DIR "${DOMIOS_DIR}/tests")

set(DOMIOS_SOURCES
    ${DOMIOS_SRC_DIR}/backend.cpp
    ${DOMIOS_SRC_DIR}/bootstrap.cpp
    ${DOMIOS_SRC_DIR}/config.cpp
    ${DOMIOS_SRC_DIR}/control_plane.cpp
    ${DOMIOS_SRC_DIR}/dit.cpp
    ${DOMIOS_SRC_DIR}/event_queue.cpp
    ${DOMIOS_SRC_DIR}/firmware.cpp
    ${DOMIOS_SRC_DIR}/lua_runtime.cpp
    ${DOMIOS_SRC_DIR}/plugin_loader.cpp
    ${DOMIOS_SRC_DIR}/presentation_plane.cpp
    ${DOMIOS_SRC_DIR}/shell.cpp
)

add_library(domios STATIC ${DOMIOS_SOURCES})
add_library(DOMWeave::domios ALIAS domios)

target_include_directories(domios
    PUBLIC
        $<BUILD_INTERFACE:${DOMIOS_INC_DIR}>
    PRIVATE
        ${DOMIOS_SRC_DIR}
)

target_link_libraries(domios
    PUBLIC
        tomlplusplus
        dynalo
        lualib
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(domios PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()

add_executable(domios_bootstrap_stub ${DOMIOS_SRC_DIR}/bootstrap_stub.cpp)
target_link_libraries(domios_bootstrap_stub PRIVATE "-Wl,--start-group" DOMWeave::domios lualib "-Wl,--end-group")
target_include_directories(domios_bootstrap_stub PRIVATE ${DOMIOS_INC_DIR} ${DOMIOS_SRC_DIR})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(domios_bootstrap_stub PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()

add_executable(mkimg ${DOMIOS_TOOLS_DIR}/mkimg/main.cpp)
target_include_directories(mkimg PRIVATE ${DOMIOS_INC_DIR} ${DOMIOS_SRC_DIR})
target_link_libraries(mkimg PRIVATE DOMWeave::domios tomlplusplus)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(mkimg PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()

set(DOMIOS_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(DOMIOS_DIT_TEMPLATE "${DOMIOS_BUILD_DIR}/dit-template.bin")
set(DOMIOS_IMAGE "${DOMIOS_BUILD_DIR}/domios.img")

add_custom_command(
    OUTPUT "${DOMIOS_DIT_TEMPLATE}"
    COMMAND $<TARGET_FILE:domios_bootstrap_stub> --emit-dit-template "${DOMIOS_DIT_TEMPLATE}"
    DEPENDS domios_bootstrap_stub
    VERBATIM
)

add_custom_command(
    OUTPUT "${DOMIOS_IMAGE}"
    COMMAND $<TARGET_FILE:mkimg>
            --dit "${DOMIOS_DIT_TEMPLATE}"
            --logo "${DOMIOS_DIR}/assets/splash_logo.txt"
            --background "${DOMIOS_DIR}/assets/splash_background.txt"
            --lua "${DOMIOS_DIR}/assets/builtin.lua"
            --manifest "${DOMIOS_DIR}/assets/manifest.toml"
            --output "${DOMIOS_IMAGE}"
    DEPENDS mkimg domios_bootstrap_stub "${DOMIOS_DIT_TEMPLATE}"
            "${DOMIOS_DIR}/assets/splash_logo.txt"
            "${DOMIOS_DIR}/assets/splash_background.txt"
            "${DOMIOS_DIR}/assets/builtin.lua"
            "${DOMIOS_DIR}/assets/manifest.toml"
    VERBATIM
)

add_custom_target(domios_image ALL DEPENDS "${DOMIOS_IMAGE}")
add_dependencies(domios_image domios_bootstrap_stub mkimg)

if(DOMVIRT_BUILD_TESTS)
add_executable(domios_tests ${DOMIOS_TEST_DIR}/domios_tests.cpp)
target_include_directories(domios_tests PRIVATE ${DOMIOS_INC_DIR} ${DOMIOS_SRC_DIR})
target_link_libraries(domios_tests PRIVATE "-Wl,--start-group" DOMWeave::domios lualib "-Wl,--end-group")
target_compile_definitions(domios_tests PRIVATE SOL_LUA_VERSION=504)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(domios_tests PRIVATE -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
endif()
add_test(NAME domios_tests COMMAND domios_tests)
endif()
