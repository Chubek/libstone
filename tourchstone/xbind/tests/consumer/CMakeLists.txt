cmake_minimum_required(VERSION 3.20)
project(TourchstoneInstalledConsumer LANGUAGES C CXX)

find_package(XBind CONFIG REQUIRED)
find_package(Tourchstone CONFIG REQUIRED)
find_package(qBFD CONFIG REQUIRED)
find_package(ArchBFD CONFIG REQUIRED)

add_executable(consumer main.cpp calls.c)
target_link_libraries(consumer PRIVATE XBind::QaMRppSpec Tourchstone::dynload
  Tourchstone::dyninvoke qbfd::qbfd ArchBFD::archbfd)
enable_testing()
add_test(NAME consumer COMMAND consumer)
