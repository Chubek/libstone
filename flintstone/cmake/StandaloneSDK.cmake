# In the root build the component targets are declared before Flintstone.
# Do not re-enable anything that the superbuild deliberately left out.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  return()
endif()

# Leave the full Limestone backend opt-in: Flintstone's scheduler/allocator
# do not require the external Limestone solver/parser dependency revisions.
option(LIBSTONE_BUILD_LIMESTONE "Include full Limestone in the Flintstone SDK" OFF)
option(LIBSTONE_BUILD_YELLOWSTONE "Include Yellowstone in the Flintstone SDK" OFF)
option(LIBSTONE_BUILD_TOURCHSTONE "Include Tourchstone in the Flintstone SDK" ON)
foreach(name IN ITEMS NOVOSTONE MOONSTONE WETSTONE WEIGHSTONE)
  option(LIBSTONE_BUILD_${name} "Include ${name} in the Flintstone SDK" ON)
endforeach()

# Keep dependency tests opt-in in the standalone Flintstone build. The
# Flintstone tests exercise the linked interfaces themselves.
foreach(name IN ITEMS LIMESTONE YELLOWSTONE NOVOSTONE MOONSTONE WETSTONE WEIGHSTONE
                      DYNLOAD DYNINVOKE XBIND)
  set(${name}_BUILD_TESTS OFF CACHE BOOL "Build ${name} tests")
endforeach()
if(NOT LIBSTONE_BUILD_YELLOWSTONE)
  set(TOURCHSTONE_BUILD_DYNLOAD OFF CACHE BOOL "Build Tourchstone dynamic loader")
  set(TOURCHSTONE_BUILD_DYNINVOKE OFF CACHE BOOL "Build Tourchstone dynamic invoker")
endif()
foreach(component IN ITEMS limestone yellowstone tourchstone novostone moonstone wetstone weighstone)
  string(TOUPPER "${component}" key)
  if(LIBSTONE_BUILD_${key})
    add_subdirectory(../${component} dependencies/${component})
  endif()
endforeach()
