# Bytecode-only build of the vendored regex engine. Its upstream glob includes
# SLJIT sources even with JIT disabled; the SLJIT submodule may be unpopulated.
if(TARGET chomsky3)
  set(GATEML_CHOMSKY_TARGET chomsky3)
else()
  file(GLOB_RECURSE CHOMSKY_SOURCES CONFIGURE_DEPENDS "${TP}/libchomsky3/src/*.c")
  list(REMOVE_ITEM CHOMSKY_SOURCES "${TP}/libchomsky3/src/vm/jit.c"
    "${TP}/libchomsky3/src/vm/jit_sljit.c")
  add_library(gateml_chomsky3 STATIC ${CHOMSKY_SOURCES})
  target_compile_features(gateml_chomsky3 PRIVATE c_std_11)
  target_include_directories(gateml_chomsky3 PUBLIC "${TP}/libchomsky3/include"
    PRIVATE "${TP}/libchomsky3/src" "${TP}/libchomsky3/third_party/unicode_names")
  if(UNIX)
    target_link_libraries(gateml_chomsky3 PRIVATE m)
  endif()
  set(GATEML_CHOMSKY_TARGET gateml_chomsky3)
endif()
