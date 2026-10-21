cmake_minimum_required(VERSION 3.20)
project(LibstoneManualFirestone LANGUAGES CXX)

find_package(Firestone 0.1 CONFIG REQUIRED COMPONENTS eqsat)
add_executable(manual-firestone main.cpp)
target_link_libraries(manual-firestone PRIVATE Firestone::eqsat)
