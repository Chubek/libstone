cmake_minimum_required(VERSION 3.20)
project(Archon LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development.Embed)

add_library(archon_embedding STATIC embedding/Archon.cpp)
add_library(Archon::Archon ALIAS archon_embedding)
set_target_properties(archon_embedding PROPERTIES
    EXPORT_NAME Archon
    POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF)
target_compile_features(archon_embedding PUBLIC cxx_std_17)
target_link_libraries(archon_embedding PUBLIC Python3::Python)
target_include_directories(archon_embedding PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

set(Archon_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/Archon")
# Preserve the position of the Python package, stdspec and the canonical
# source resources copied into the generated Translate.py artifact.
set(Archon_DATA_DIR "${CMAKE_INSTALL_DATADIR}/archon")
set(Archon_PYTHON_DIR "${Archon_DATA_DIR}/cornerstone/Archon")

configure_package_config_file(embedding/ArchonConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ArchonConfig.cmake"
    INSTALL_DESTINATION "${Archon_PACKAGE_DIR}"
    PATH_VARS Archon_PYTHON_DIR)

install(TARGETS archon_embedding EXPORT ArchonTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT ArchonTargets NAMESPACE Archon:: DESTINATION "${Archon_PACKAGE_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ArchonConfig.cmake"
    DESTINATION "${Archon_PACKAGE_DIR}")
install(DIRECTORY archon DESTINATION "${Archon_PYTHON_DIR}"
    FILES_MATCHING PATTERN "*.py" PATTERN "*.pl" PATTERN "*.c" PATTERN "*.h" PATTERN "*.c.in"
    PATTERN "__pycache__" EXCLUDE)
install(DIRECTORY stdspec DESTINATION "${Archon_PYTHON_DIR}"
    FILES_MATCHING PATTERN "*.arch")
install(FILES ../ASDL/scripts/asdl.pl DESTINATION "${Archon_DATA_DIR}/cornerstone/ASDL/scripts")
install(FILES ../Aurocks/scripts/aurocks.pl DESTINATION "${Archon_DATA_DIR}/cornerstone/Aurocks/scripts")
install(FILES ../../LICENSE DESTINATION "${Archon_DATA_DIR}")
