set(dir "${BINARY}/canonical-cli")
file(MAKE_DIRECTORY "${dir}")
file(WRITE "${dir}/dep.wit" [=[
package test:dep@1.0.0;
interface api {
    record item { contents: list<u8>, }
}
]=])
file(WRITE "${dir}/app.wit" [=[
world app {
    use test:dep/api@1.0.0.{item};
    import collect: func(x: item) -> string;
    export respond: func(x: item) -> string;
}
]=])
execute_process(COMMAND "${WIT2C}" --canonical --dep "${dir}/dep.wit" "${dir}/app.wit"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "wit2c --canonical failed (${status}): ${error}")
endif()
string(FIND "${output}" "lower wit_world_G_app_I_collect (i32, i32, i32) -> () [result out-pointer]" imported)
string(FIND "${output}" "lift wit_world_G_app_E_respond (i32, i32) -> (i32) [result pointer]" exported)
if(imported LESS 0 OR exported LESS 0)
  message(FATAL_ERROR "unexpected Canonical ABI signatures:\n${output}")
endif()

execute_process(COMMAND "${WIT2C}" --canonical "${dir}/app.wit"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(status STREQUAL "0" OR NOT output STREQUAL "" OR NOT error MATCHES "test:dep")
  message(FATAL_ERROR "missing dependency was not diagnosed (${status}): ${error}")
endif()
