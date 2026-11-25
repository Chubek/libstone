#include "wasmutils/WITParser.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace wasm = limestone::wasmutils;
namespace {

constexpr std::string_view fixture = R"WIT(
package demo:host@1.2.3;

interface api {
    record packet { title: string, data: bytes, %union: utf-char }
    type bytes = list<u8>;
    type utf-char = char;
    variant response { failed(failure), empty, packet(packet) }
    enum failure { retry, rejected }
    type maybe-packet = option<packet>;
    type exchange = result<maybe-packet, response>;
    type error-only = result<_, failure>;
    type ack = result;
    type point = tuple<s32, s32>;
    type nested-list = list<option<u8>>;
    flags controls {
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9,
        f10, f11, f12, f13, f14, f15, f16, f17, f18, f19,
        f20, f21, f22, f23, f24, f25, f26, f27, f28, f29,
        f30, f31, %f32, f33,
    }
    resource socket {
        constructor(initial: packet);
        read-packet: func(data: bytes) -> exchange;
        get-default: static func() -> own<socket>;
    }
    write-packet: func(handle: borrow<socket>, value: packet, view: list<u8>) -> exchange;
    consume: func(arg: own<socket>) -> ack;
}

interface bridge {
    use api.{packet as outgoing};
    type wire = list<u8>;
    pass: func(value: outgoing);
}

world provider {
    use api.{packet};
    import api;
    import fallback: func(value: packet) -> u32;
    export handler: interface {
        use api.{packet};
        inspect: func(value: packet) -> string;
    }
}

world application {
    import demo:host/api@1.2.3;
    include provider with { fallback as fallback-renamed, packet as request }
    export done: func();
}
)WIT";

void check(bool ok, std::string_view message) {
    if (!ok)
        throw std::runtime_error(std::string(message));
}

void contains(const std::string& header, std::string_view text) {
    if (header.find(text) == std::string::npos)
        throw std::runtime_error("missing from generated C: " + std::string(text));
}

void test_headers() {
    const auto parsed = wasm::parse_wit(fixture);
    const auto header = wasm::emit_wit_c(parsed);
    check(header == wasm::wit_to_c(fixture), "WIT C emission must be deterministic");
    check(header == wasm::wit_to_c("\n/* locations differ */ " + std::string(fixture)),
          "comments/outside whitespace changed generated C");
    contains(header, "ANSI C host declarations only");
    contains(header, "Component Model Canonical ABI");
    contains(header, "typedef struct wit_demo_W_host_W_v1v2v3_W_api_T_packet");
    contains(header, "wit_host_string wit_field_title;");
    contains(header, "const unsigned char *data;");
    contains(header, "wit_host_A_");
    contains(header, " wit_arg_view");
    contains(header, "wit_demo_W_host_W_v1v2v3_W_api_T_utf_char wit_field_union;");
    check(header.find(" wit_demo_W_host_W_v1v2v3_W_api_T_bytes;") <
              header.find("typedef struct wit_demo_W_host_W_v1v2v3_W_api_T_packet"),
          "type dependencies must be emitted before use");
    contains(header, "wit_demo_W_host_W_v1v2v3_W_api_T_response_C_empty 1UL");
    contains(header, "wit_demo_W_host_W_v1v2v3_W_api_T_controls_F_f31_MASK (1UL << 31)");
    contains(header, "wit_demo_W_host_W_v1v2v3_W_api_T_controls_F_f32_WORD 1UL");
    contains(header, "wit_demo_W_host_W_v1v2v3_W_api_T_socket_M_read_packet("
                     "wit_demo_W_host_W_v1v2v3_W_api_T_socket_B wit_self,");
    contains(header, "wit_demo_W_host_W_v1v2v3_W_api_T_socket_S_get_default(void);");
    contains(header, "typedef wit_demo_W_host_W_v1v2v3_W_api_T_packet "
                     "wit_demo_W_host_W_v1v2v3_W_bridge_T_outgoing;");
    contains(header, " wit_demo_W_host_W_v1v2v3_W_bridge_T_wire;");
    contains(header, "wit_demo_W_host_W_v1v2v3_W_world_G_provider_I_path_P_api_W_consume(");
    contains(header, "wit_demo_W_host_W_v1v2v3_W_world_G_provider_E_handler_W_inspect(");
    contains(header, "wit_demo_W_host_W_v1v2v3_W_world_G_application_I_fallback_renamed(");
    contains(header, "wit_demo_W_host_W_v1v2v3_W_world_G_application_T_request;");
    constexpr auto shared_import = "wit_demo_W_host_W_v1v2v3_W_world_G_application_I_path_P_api_W_consume(";
    contains(header, shared_import);
    check(header.find(shared_import) == header.rfind(shared_import),
          "a local interface path shared by included worlds should be declared only once");

    const auto scalar = wasm::wit_to_c(
        "interface arithmetic { add: func(%type: u32, %enum: s32) -> u32; }");
    contains(scalar, "wit_host_u32\nwit_arithmetic_W_add(wit_host_u32 wit_arg_type,");
    contains(scalar, "wit_host_s32 wit_arg_enum);");
    const auto prerelease = wasm::wit_to_c(
        "package demo:host@1.2.3-rc.1+debug; interface arithmetic { neg: func(); }");
    contains(prerelease, "wit_demo_W_host_W_v1v2v3_V_2D72632E312B6465627567_W_arithmetic_W_neg(void);");

    const auto standalone_world = wasm::wit_to_c("world local { export run: func(); }");
    contains(standalone_world, "void\nwit_world_G_local_E_run(void);");
}

void test_unresolved_dependency() {
    const auto document = wasm::parse_wit(
        "interface api {\n    use remote:pkg/types@1.0.0.{ external };\n"
        "    send: func(value: external);\n}\n");
    try {
        static_cast<void>(wasm::emit_wit_c(document));
    } catch (const wasm::WitError& error) {
        check(error.location.line == 2 && error.location.column != 0,
              "unresolved dependency should have a source location");
        check(std::string_view(error.what()).find("remote:pkg/types@1.0.0") != std::string_view::npos,
              "diagnostic should name the unresolved dependency");
        return;
    }
    throw std::runtime_error("emitted a host declaration for an unresolved external dependency");
}

void test_malformed_ast() {
    auto document = wasm::parse_wit("interface api { type names = list<string>; }");
    auto& type = *document.interfaces.front().types.front().alias;
    type.arguments.clear();
    try {
        static_cast<void>(wasm::emit_wit_c(document));
    } catch (const wasm::WitError& error) {
        check(error.location.offset == type.location.offset,
              "malformed compound type diagnostic should identify the type");
        return;
    }
    throw std::runtime_error("emitted a host declaration for a malformed list AST");
}

void reject(const wasm::WitDocument& root, std::span<const wasm::WitDocument> dependencies,
            std::string_view diagnostic, std::optional<std::size_t> dependency_index = {}) {
    try {
        static_cast<void>(wasm::emit_wit_c(root, dependencies));
    } catch (const wasm::WitError& error) {
        if (std::string_view(error.what()).find(diagnostic) == std::string_view::npos)
            throw std::runtime_error("unexpected dependency diagnostic: " + std::string(error.what()));
        check(error.dependency_index == dependency_index, "wrong dependency diagnostic source");
        return;
    }
    throw std::runtime_error("did not reject dependency input: " + std::string(diagnostic));
}

void test_dependency_headers() {
    const auto model1 = wasm::parse_wit(R"WIT(
        package shapes:domain@1.0.0;
        interface model {
            record row { id: u32 }
            resource socket { send: func(value: row); }
        }
        interface unrelated { skip: func(); }
    )WIT");
    const auto model2 = wasm::parse_wit(R"WIT(
        package shapes:domain@2.0.0;
        interface model {
            record row { text: string }
            append: func(value: row);
        }
    )WIT");
    const auto transport = wasm::parse_wit(R"WIT(
        package transport:wire@2.0.0;
        use shapes:domain/model@1.0.0 as local-model;
        interface io {
            use local-model.{row as view, socket};
            publish: func(value: view);
        }
        world producer {
            use local-model.{row as late};
            import io;
            export flush: func(value: late);
            export control: interface {
                use io.{view};
                state: func() -> view;
            }
        }
    )WIT");
    const auto root = wasm::parse_wit(R"WIT(
        package client:app@3.0.0;
        use transport:wire/producer@2.0.0 as base;
        interface entry {
            use transport:wire/io@2.0.0.{view as row, socket};
            forward: func(handle: borrow<socket>, value: row);
        }
        interface io { shadowed: func(); }
        world app {
            import shapes:domain/model@1.0.0;
            import shapes:domain/model@2.0.0;
            include base with { late as line, flush as finish }
        }
    )WIT");

    const std::array deps{model2, transport, model1};
    const auto header = wasm::emit_wit_c(root, deps);
    check(header == wasm::emit_wit_c(root, std::array{model1, model2, transport}),
          "dependency order changed generated C");
    contains(header, "typedef wit_shapes_W_domain_W_v1v0v0_W_model_T_row "
                     "wit_transport_W_wire_W_v2v0v0_W_io_T_view;");
    contains(header, "typedef wit_transport_W_wire_W_v2v0v0_W_io_T_view "
                     "wit_client_W_app_W_v3v0v0_W_entry_T_row;");
    contains(header, "wit_shapes_W_domain_W_v1v0v0_W_model_T_socket_B wit_arg_handle");
    contains(header, "wit_client_W_app_W_v3v0v0_W_world_G_app_I_path_P_wit_transport_W_wire_W_v2v0v0_W_io_W_publish(");
    contains(header, "wit_client_W_app_W_v3v0v0_W_world_G_app_I_path_P_wit_shapes_W_domain_W_v2v0v0_W_model_W_append(");
    contains(header, "wit_client_W_app_W_v3v0v0_W_world_G_app_E_finish(");
    contains(header, "wit_client_W_app_W_v3v0v0_W_world_G_app_E_control_W_state(");
    contains(header, "wit_client_W_app_W_v3v0v0_W_world_G_app_T_line;");
    check(header.find("unrelated") == std::string::npos, "an unrelated dependency declaration leaked");
    check(header.find("_G_app_I_path_P_io_W_shadowed") == std::string::npos, "a dependency path resolved in root scope");

    reject(root, std::array{model2, transport}, "shapes:domain/model@1.0.0", 1);
    reject(root, std::array{model1, transport, model1}, "duplicate WIT package 'shapes:domain@1.0.0'", 2);
    reject(root, std::array{root}, "duplicate WIT package 'client:app@3.0.0'", 0);
    reject(root, std::array{wasm::parse_wit("interface nop {}")}, "must declare its package", 0);

    const auto unversioned = wasm::parse_wit("interface r { use shapes:domain/model.{row}; }");
    reject(unversioned, std::array{model1}, "shapes:domain/model");
    const auto wrong_case = wasm::parse_wit("interface r { use shapes:domain/MODEL@1.0.0.{row}; }");
    reject(wrong_case, std::array{model1}, "cannot find WIT interface");
    const auto wrong_type = wasm::parse_wit("interface r { use shapes:domain/model@1.0.0.{unknown}; }");
    reject(wrong_type, std::array{model1}, "unknown WIT type 'unknown'");
    const auto prefix_collision = wasm::parse_wit("package SHAPES:domain@1.0.0; interface x {}");
    reject(root, std::array{model1, prefix_collision}, "conflicting C prefix", 1);
}

void test_dependency_errors() {
    const auto a = wasm::parse_wit(R"WIT(
        package cycle:a;
        interface a { use cycle:b/b.{y}; type x = u8; }
    )WIT");
    const auto b = wasm::parse_wit(R"WIT(
        package cycle:b;
        interface b { use cycle:a/a.{x}; type y = u8; }
    )WIT");
    reject(a, std::array{b}, "cyclic WIT interface use");
    const auto x = wasm::parse_wit("package cycle:x; world x { include cycle:y/y; }");
    const auto y = wasm::parse_wit("package cycle:y; world y { include cycle:x/x; }");
    reject(x, std::array{y}, "cyclic WIT world include", 0);

    const auto base = wasm::parse_wit(R"WIT(
        package outside:dep;
        world base { export duplicate: func(); }
        interface r {
            resource pipe;
            type view = option<borrow<pipe>>;
            type owned = own<pipe>;
        }
    )WIT");
    const auto duplicate = wasm::parse_wit("world main { include outside:dep/base; export duplicate: func(); }");
    reject(duplicate, std::array{base}, "duplicate WIT world member 'duplicate'");
    const auto bad_rename = wasm::parse_wit("world main { include outside:dep/base with { missing as renamed } }");
    reject(bad_rename, std::array{base}, "unknown named WIT include item 'missing'");
    const auto wrong_kind = wasm::parse_wit("world main { include outside:dep/r; }");
    reject(wrong_kind, std::array{base}, "cannot find WIT world 'outside:dep/r'");
    const auto borrow = wasm::parse_wit("interface f { use outside:dep/r.{view}; bad: func() -> view; }");
    reject(borrow, std::array{base}, "cannot return a borrowed handle");
    const auto bad_handle = wasm::parse_wit("interface f { use outside:dep/r.{owned}; bad: func(v: borrow<owned>); }");
    reject(bad_handle, std::array{base}, "handle target must be a resource");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--fixture") {
            std::cout << fixture;
            return !std::cout;
        }
        if (argc != 1)
            return 2;
        test_headers();
        test_unresolved_dependency();
        test_malformed_ast();
        test_dependency_headers();
        test_dependency_errors();
    } catch (const std::exception& error) {
        std::cerr << "WIT C test: " << error.what() << '\n';
        return 1;
    }
}
