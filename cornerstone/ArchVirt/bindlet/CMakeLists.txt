cmake_minimum_required(VERSION 3.15)
project(bindlets VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ═══════════════════════════════════════════════════════════════════
#  Dependencies
# ═══════════════════════════════════════════════════════════════════

# DomPyBind from the enclosing Libstone source tree, also for standalone builds.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../tourchstone/xbind/dombind/dompybind/cmake/DomPyBind.cmake")


# Python 3 development libraries
find_package(Python 3.13 REQUIRED COMPONENTS Interpreter Development Development.Module)

# ═══════════════════════════════════════════════════════════════════
#  Bindlets shared library
# ═══════════════════════════════════════════════════════════════════

add_library(bindlets SHARED
    src/domainlet_wrapper.cpp
    src/bindlets_api.cpp
)

target_include_directories(bindlets
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${Python_INCLUDE_DIRS}
)

target_link_libraries(bindlets
    PUBLIC
        DomPyBind::DomPyBind
        Python::Python
)

# ═══════════════════════════════════════════════════════════════════
#  Install rules
# ═══════════════════════════════════════════════════════════════════

include(GNUInstallDirs)

install(TARGETS bindlets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# ═══════════════════════════════════════════════════════════════════
#  Platform-specific settings
# ═══════════════════════════════════════════════════════════════════

if(APPLE)
    set_target_properties(bindlets PROPERTIES
        INSTALL_RPATH "@loader_path"
        MACOSX_RPATH ON
    )
elseif(UNIX)
    set_target_properties(bindlets PROPERTIES
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# ═══════════════════════════════════════════════════════════════════
#  Compiler warnings
# ═══════════════════════════════════════════════════════════════════

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(bindlets PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ═══════════════════════════════════════════════════════════════════
#  Native integration tests
# ═══════════════════════════════════════════════════════════════════

get_filename_component(BINDLET_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
option(BINDLET_BUILD_TESTS "Build bindlet contract tests" OFF)
if(DOMWEAVE_BUILD_TESTS OR BINDLET_BUILD_TESTS)
    enable_testing()

    add_executable(bindlets_c_api_test
        ${BINDLET_REPO_ROOT}/tests/bindlet-test-suite/bindlets_api_test.c
    )
    target_link_libraries(bindlets_c_api_test PRIVATE bindlets)
    target_compile_definitions(bindlets_c_api_test PRIVATE
        DOMWEAVE_SOURCE_DIR="${BINDLET_REPO_ROOT}"
    )
    set_target_properties(bindlets_c_api_test PROPERTIES
        LINKER_LANGUAGE CXX
    )

    add_test(NAME bindlets-c-api COMMAND bindlets_c_api_test)
    set_tests_properties(bindlets-c-api PROPERTIES
        ENVIRONMENT
            "PYTHONPATH=${BINDLET_REPO_ROOT}/tests/bindlet-test-suite:$ENV{PYTHONPATH}"
    )
endif()
