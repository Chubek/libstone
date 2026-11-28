add_library(mahaksdk SHARED src/PluginSdk.cpp)
add_library(Moonstone::MahakSdk ALIAS mahaksdk)
target_compile_features(mahaksdk PUBLIC cxx_std_20)
target_include_directories(mahaksdk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(mahaksdk PUBLIC moonstone_core)
if(MSVC)
  target_compile_options(mahaksdk PRIVATE /W4)
else()
  target_compile_options(mahaksdk PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(CMAKE_DL_LIBS AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(moonstone_mahak PUBLIC ${CMAKE_DL_LIBS})
endif()

install(TARGETS mahaksdk LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/mahak/Plugin.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mahak)

if(TARGET moonstone_test_mahak_plugin)
  add_dependencies(moonstone_test_mahak_plugin mahaksdk)
  target_compile_definitions(moonstone_test_mahak_plugin PRIVATE
    MAHAK_PLUGIN_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}/include"
    MAHAK_PLUGIN_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plugin/oracle.cpp"
    MAHAK_SDK_DIR="$<TARGET_FILE_DIR:mahaksdk>")
endif()
