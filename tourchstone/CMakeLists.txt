cmake_minimum_required(VERSION 3.20)
project(Tourchstone VERSION 0.1.0 LANGUAGES C CXX)

include(../cmake/LibstoneBuild.cmake)

option(TOURCHSTONE_BUILD_XBIND "Build the XBind generator and tool" ON)
option(TOURCHSTONE_BUILD_DYNLOAD "Build the dynload loader/inspection library and tool" ON)
option(TOURCHSTONE_BUILD_DYNINVOKE "Build the dyninvoke foreign-call library and tool" ON)

if(TOURCHSTONE_BUILD_XBIND)
  add_subdirectory(xbind)
endif()

if(TOURCHSTONE_BUILD_DYNLOAD)
  add_subdirectory(dynload)
endif()

if(TOURCHSTONE_BUILD_DYNINVOKE)
  add_subdirectory(dyninvoke)
endif()
