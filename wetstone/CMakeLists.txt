cmake_minimum_required(VERSION 3.20)
project(Wetstone VERSION 0.1.0 LANGUAGES CXX)
include(CTest)
include(GNUInstallDirs)
add_library(wetstone INTERFACE)
add_library(Wetstone::wetstone ALIAS wetstone)
target_compile_features(wetstone INTERFACE cxx_std_20)
target_include_directories(wetstone INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(wetstone_alias_analysis STATIC
    alias-analysis/TypeDAG.cpp
    alias-analysis/MemoryLocation.cpp
    alias-analysis/CallSite.cpp
    alias-analysis/MustMay.cpp
    alias-analysis/DependenceTypes.cpp
    alias-analysis/LoopDependenceAnalysis.cpp
    alias-analysis/DirectAnalysis.cpp
    alias-analysis/RegionBasedAnalysis.cpp)
add_library(Wetstone::alias_analysis ALIAS wetstone_alias_analysis)
set_target_properties(wetstone_alias_analysis PROPERTIES
    EXPORT_NAME alias_analysis
    POSITION_INDEPENDENT_CODE ON)
target_link_libraries(wetstone_alias_analysis PUBLIC wetstone)
# Use LibStone's checked out vendored dependencies, if available.
get_filename_component(LIBSTONE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(WETSTONE_THIRD_PARTY_DIR "${LIBSTONE_ROOT}/third_party" CACHE PATH "LibStone vendor root")
option(WETSTONE_USE_SATIE "Use third_party/satie for SAT and LIA" ON)
option(WETSTONE_USE_EQUINOX "Use third_party/equinox-ng for equality saturation" ON)
option(WETSTONE_USE_FLINTSTONE "Enable Flintstone/Firestone adapters when the sources exist" ON)
if(WETSTONE_USE_SATIE)
    set(wetstone_satie "${WETSTONE_THIRD_PARTY_DIR}/satie")
    if(TARGET satie::satie)
        target_link_libraries(wetstone INTERFACE $<BUILD_INTERFACE:satie::satie>)
    elseif(EXISTS "${wetstone_satie}/include/SatieCDCL.hpp" AND
           EXISTS "${wetstone_satie}/src/SatieMemory.cpp" AND
           EXISTS "${wetstone_satie}/third_party/memtkx/include/dommemtk/space/free_list.hpp")
        find_package(Threads REQUIRED)
        add_library(wetstone_satie_runtime STATIC "${wetstone_satie}/src/SatieMemory.cpp")
        set_target_properties(wetstone_satie_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_compile_features(wetstone_satie_runtime PUBLIC cxx_std_20)
        target_include_directories(wetstone_satie_runtime SYSTEM PUBLIC "${wetstone_satie}/include"
            PRIVATE "${wetstone_satie}/third_party/memtkx/include")
        target_link_libraries(wetstone_satie_runtime PRIVATE Threads::Threads)
        target_link_libraries(wetstone INTERFACE $<BUILD_INTERFACE:wetstone_satie_runtime>)
    endif()
endif()
if(WETSTONE_USE_EQUINOX AND EXISTS "${WETSTONE_THIRD_PARTY_DIR}/equinox-ng/EquinoxNG.hpp")
    target_include_directories(wetstone INTERFACE $<BUILD_INTERFACE:${WETSTONE_THIRD_PARTY_DIR}/equinox-ng>)
    # Wetstone uses the e-graph API, not the optional DSL layer, whose
    # utilities share names with the copy in Satie.
    target_compile_definitions(wetstone INTERFACE EQUINOXNG_DISABLE_DSLUTILS)
endif()
if(WETSTONE_USE_FLINTSTONE AND EXISTS "${LIBSTONE_ROOT}/flintstone/Flintstone.hpp")
    target_include_directories(wetstone INTERFACE
        $<BUILD_INTERFACE:${LIBSTONE_ROOT}/flintstone>
        $<BUILD_INTERFACE:${LIBSTONE_ROOT}/firestone>)
endif()
if(NOT DEFINED LIBSTONE_BUILD_TESTS)
    set(LIBSTONE_BUILD_TESTS "${BUILD_TESTING}")
endif()
option(WETSTONE_BUILD_TESTS "Build Wetstone tests" "${LIBSTONE_BUILD_TESTS}")
if(WETSTONE_BUILD_TESTS)
    foreach(test IN ITEMS core cfg domains interprocedural symbolic pointer soundness typed
                          alias_foundations alias_algorithms)
        add_executable(wetstone-${test}-test tests/test_${test}.cpp)
        if(test MATCHES "^alias_")
            target_link_libraries(wetstone-${test}-test PRIVATE Wetstone::alias_analysis)
        else()
            target_link_libraries(wetstone-${test}-test PRIVATE wetstone)
        endif()
        # The suite is assert-based; keep assertions live even in Release builds
        # (where -DNDEBUG would otherwise silently disable every check).
        if(MSVC)
            target_compile_options(wetstone-${test}-test PRIVATE /UNDEBUG)
        else()
            target_compile_options(wetstone-${test}-test PRIVATE -UNDEBUG)
        endif()
        add_test(NAME wetstone.${test} COMMAND wetstone-${test}-test)
    endforeach()
endif()
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/wetstone"
    FILES_MATCHING PATTERN "*.hpp")
install(TARGETS wetstone EXPORT WetstoneTargets)
install(TARGETS wetstone_alias_analysis EXPORT WetstoneTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(EXPORT WetstoneTargets NAMESPACE Wetstone:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Wetstone")
