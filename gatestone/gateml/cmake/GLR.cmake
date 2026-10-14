# Build the requested top-level libglr implementation with libstone's populated
# klib/ctl copies. libglr's nested klib/ctl submodules can be empty in this tree.
# No dependency source is copied or modified.
if(TARGET libglr)
  set(GATEML_GLR_TARGET libglr)
else()
  find_package(Threads REQUIRED)
  set(HAVE_LMDB 0)
  set(HAVE_LIBMDBX 0)
  configure_file("${TP}/libglr/cmake/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/glr-config/glr/config.h")
  file(GLOB SFSEXP_SOURCES CONFIGURE_DEPENDS "${TP}/libglr/third_party/sfsexp/src/*.c")
  add_library(gateml_sfsexp STATIC ${SFSEXP_SOURCES})
  target_include_directories(gateml_sfsexp PUBLIC "${TP}/libglr/third_party/sfsexp/src")
  file(GLOB GLR_SOURCES CONFIGURE_DEPENDS "${TP}/libglr/src/glr/*.c")
  list(REMOVE_ITEM GLR_SOURCES "${TP}/libglr/src/glr/cache.c")
  add_library(gateml_glr STATIC ${GLR_SOURCES} "${TP}/klib/kalloc.c" "${TP}/klib/kthread.c")
  target_include_directories(gateml_glr PUBLIC "${TP}/libglr/include"
    "${CMAKE_CURRENT_BINARY_DIR}/glr-config" PRIVATE "${TP}" "${TP}/ctl" "${TP}/ctl/ctl")
  target_link_libraries(gateml_glr PUBLIC gateml_sfsexp Threads::Threads)
  set(GATEML_GLR_TARGET gateml_glr)
endif()
