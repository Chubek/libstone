set(dir "${BINARY}/wit-c90")
file(MAKE_DIRECTORY "${dir}")
separate_arguments(cflags NATIVE_COMMAND "${CFLAGS}")
separate_arguments(link_flags NATIVE_COMMAND "${LINK_FLAGS}")

function(run)
  execute_process(COMMAND ${ARGV} WORKING_DIRECTORY "${dir}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "${ARGV} failed (${status}):\n${output}${errors}")
  endif()
endfunction()

function(output_to name)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${dir}"
    RESULT_VARIABLE status OUTPUT_FILE "${dir}/${name}" ERROR_VARIABLE errors)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "${ARGN} failed (${status}):\n${errors}")
  endif()
endfunction()

output_to(fixture.wit "${WIT_C_TEST}" --fixture)
output_to(bindings.h "${WIT2C}" fixture.wit)
file(WRITE "${dir}/second.wit"
  "package demo:other@1.2.3; interface api { type bytes = list<u8>; run: func(data: bytes); }\n")
output_to(second.h "${WIT2C}" second.wit)

file(WRITE "${dir}/messages.wit" [=[
package shared:messages@2.0.0;
interface schema {
    record message { id: u32 }
    type messages = list<message>;
    unused: func();
}
]=])
file(WRITE "${dir}/service.wit" [=[
package shared:service;
use shared:messages/schema@2.0.0 as schema;
interface writer {
    use schema.{message, messages};
    send: func(value: message) -> u32;
    enqueue: func(values: messages);
}
world server {
    use schema.{message};
    export writer;
    export process: func(value: message) -> u32;
}
]=])
file(WRITE "${dir}/application.wit" [=[
package client:application;
world service {
    include shared:service/server with { process as process-message }
}
]=])
output_to(application.h "${WIT2C}" --dep messages.wit --dep service.wit application.wit)
output_to(reordered.h "${WIT2C}" --dep service.wit --dep messages.wit application.wit)
output_to(messages.h "${WIT2C}" messages.wit)
output_to(service.h "${WIT2C}" --dep messages.wit service.wit)
run("${CMAKE_COMMAND}" -E compare_files application.h reordered.h)

execute_process(COMMAND "${WIT2C}" --dep messages.wit --dep service.wit -
  WORKING_DIRECTORY "${dir}" INPUT_FILE "${dir}/application.wit" OUTPUT_FILE "${dir}/stdin.h"
  RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "wit2c with dependencies and stdin failed (${status}):\n${errors}")
endif()
run("${CMAKE_COMMAND}" -E compare_files application.h stdin.h)

execute_process(COMMAND "${WIT2C}" --dep service.wit application.wit WORKING_DIRECTORY "${dir}"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT status STREQUAL "1" OR NOT output STREQUAL "" OR
   NOT errors MATCHES "service\\.wit:[0-9]+:[0-9]+: error: .*shared:messages/schema@2\\.0\\.0")
  message(FATAL_ERROR "expected dependency-site error, got (${status}):\n${output}${errors}")
endif()

file(WRITE "${dir}/driver.c" [=[
#include "second.h"
#include "bindings.h"
#include "bindings.h"
#include "application.h"
#include "messages.h"
#include "service.h"

typedef wit_demo_W_host_W_v1v2v3_W_api_T_packet packet;
typedef wit_demo_W_host_W_v1v2v3_W_api_T_bytes bytes;
typedef wit_demo_W_host_W_v1v2v3_W_api_T_socket socket_handle;
typedef wit_demo_W_host_W_v1v2v3_W_api_T_socket_B socket_view;
typedef wit_demo_W_host_W_v1v2v3_W_api_T_exchange exchange;
typedef wit_demo_W_host_W_v1v2v3_W_api_T_ack ack;
typedef wit_demo_W_host_W_v1v2v3_W_api_T_error_only error_only;
typedef wit_demo_W_host_W_v1v2v3_W_api_T_point point;
typedef wit_demo_W_host_W_v1v2v3_W_api_T_controls controls;

struct wit_demo_W_host_W_v1v2v3_W_api_T_socket_H {
    packet current;
};

static struct wit_demo_W_host_W_v1v2v3_W_api_T_socket_H socket_storage;

socket_handle
wit_demo_W_host_W_v1v2v3_W_api_T_socket_N_new(packet initial)
{
    socket_storage.current = initial;
    return &socket_storage;
}

exchange
wit_demo_W_host_W_v1v2v3_W_api_T_socket_M_read_packet(socket_view self, bytes view)
{
    exchange reply;
    if (view.len == 0) {
        reply.tag = WIT_HOST_RESULT_ERR;
        reply.payload.err.tag = wit_demo_W_host_W_v1v2v3_W_api_T_response_C_failed;
        reply.payload.err.payload.wit_case_failed = wit_demo_W_host_W_v1v2v3_W_api_T_failure_C_retry;
    } else {
        reply.tag = WIT_HOST_RESULT_OK;
        reply.payload.ok.tag = WIT_HOST_OPTION_SOME;
        reply.payload.ok.value = self->current;
    }
    return reply;
}

ack
wit_demo_W_host_W_v1v2v3_W_api_W_consume(socket_handle arg)
{
    ack reply;
    arg->current.wit_field_title.len = 0;
    reply.tag = WIT_HOST_RESULT_OK;
    return reply;
}

wit_host_u32
wit_demo_W_host_W_v1v2v3_W_world_G_application_I_fallback_renamed(packet value)
{
    return value.wit_field_union;
}

wit_host_u32
wit_client_W_application_W_world_G_service_E_process_message(wit_shared_W_messages_W_v2v0v0_W_schema_T_message value)
{
    return value.wit_field_id + 2;
}

wit_host_u32
wit_client_W_application_W_world_G_service_E_path_P_wit_shared_W_service_W_writer_W_send(
    wit_shared_W_service_W_writer_T_message value)
{
    return value.wit_field_id + 1;
}

int main(void)
{
    static const char title[] = {'a', '\0', 'b'};
    static const unsigned char data[] = {99};
    packet input = {{title, sizeof title}, {data, sizeof data}, 65};
    bytes empty = {0, 0};
    controls state = {{0, 0}};
    point coordinates = {10, -20};
    wit_client_W_application_W_world_G_service_T_message message = {40};
    error_only failed;
    socket_handle socket = wit_demo_W_host_W_v1v2v3_W_api_T_socket_N_new(input);
    exchange reply = wit_demo_W_host_W_v1v2v3_W_api_T_socket_M_read_packet(socket, input.wit_field_data);
    exchange failure = wit_demo_W_host_W_v1v2v3_W_api_T_socket_M_read_packet(socket, empty);
    ack status = wit_demo_W_host_W_v1v2v3_W_api_W_consume(socket);
    state.words[wit_demo_W_host_W_v1v2v3_W_api_T_controls_F_f31_WORD] |=
        wit_demo_W_host_W_v1v2v3_W_api_T_controls_F_f31_MASK;
    state.words[wit_demo_W_host_W_v1v2v3_W_api_T_controls_F_f32_WORD] |=
        wit_demo_W_host_W_v1v2v3_W_api_T_controls_F_f32_MASK;
    failed.tag = WIT_HOST_RESULT_ERR;
    failed.payload.err = wit_demo_W_host_W_v1v2v3_W_api_T_failure_C_rejected;

    return reply.tag != WIT_HOST_RESULT_OK || reply.payload.ok.tag != WIT_HOST_OPTION_SOME ||
           reply.payload.ok.value.wit_field_title.len != 3 ||
           reply.payload.ok.value.wit_field_title.data[2] != 'b' ||
           reply.payload.ok.value.wit_field_data.len != 1 ||
           reply.payload.ok.value.wit_field_data.data[0] != 99 ||
           failure.tag != WIT_HOST_RESULT_ERR ||
           failure.payload.err.tag != wit_demo_W_host_W_v1v2v3_W_api_T_response_C_failed ||
           failure.payload.err.payload.wit_case_failed != wit_demo_W_host_W_v1v2v3_W_api_T_failure_C_retry ||
           state.words[0] != 0x80000000UL || state.words[1] != 1UL ||
           coordinates.wit_item_0 != 10 || coordinates.wit_item_1 != -20 ||
           failed.tag != WIT_HOST_RESULT_ERR || failed.payload.err != 1UL ||
           status.tag != WIT_HOST_RESULT_OK || socket->current.wit_field_title.len != 0 ||
           wit_client_W_application_W_world_G_service_E_process_message(message) != 42 ||
           wit_client_W_application_W_world_G_service_E_path_P_wit_shared_W_service_W_writer_W_send(message) != 41 ||
           wit_demo_W_host_W_v1v2v3_W_world_G_application_I_fallback_renamed(input) != 65;
}
]=])

run("${CC}" ${cflags} -std=c90 -pedantic-errors -Wall -Wextra -Werror driver.c
  ${link_flags} -o "wit-c90${EXE_SUFFIX}")
run("${dir}/wit-c90${EXE_SUFFIX}")
