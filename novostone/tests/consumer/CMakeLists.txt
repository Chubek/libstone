cmake_minimum_required(VERSION 3.20)
project(NovostoneConsumer LANGUAGES CXX)
find_package(Novostone CONFIG REQUIRED)
add_executable(novostone-consumer main.cpp)
target_link_libraries(novostone-consumer PRIVATE
  Novostone::CostCC Novostone::SpeculativeIR Novostone::PCC
  Novostone::ActionSemantics)
if(TARGET Novostone::PCCPackage)
  target_link_libraries(novostone-consumer PRIVATE Novostone::PCCPackage)
  target_compile_definitions(novostone-consumer PRIVATE TEST_PCC_PACKAGE=1)
endif()
enable_testing()
add_test(NAME installed-novostone COMMAND novostone-consumer)
