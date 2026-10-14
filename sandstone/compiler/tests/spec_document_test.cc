#include "domsymascc/domsymascc.hpp"
#include <cstdlib>
#include <iostream>

namespace {
void check(bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
}

int main() {
    domsymas::encoding::Program encoding;
    domsymas::encoding::Outcome bytes;
    std::string diagnostic;
    check(domsymas::encoding::parse("emit64le(0xDC50000000000000); emit64be(0xFFFFFFFFFFFFFFFF);",
                                    encoding, diagnostic), diagnostic.c_str());
    check(domsymas::encoding::execute(encoding, {}, {}, bytes, diagnostic), diagnostic.c_str());
    check(bytes.bytes == std::vector<uint8_t>({0,0,0,0,0,0,0x50,0xDC,
                                             255,255,255,255,255,255,255,255}),
          "64-bit hexadecimal opcode lost bits");
    check(!domsymas::encoding::parse("emit64le(0x10000000000000000);", encoding, diagnostic),
          "oversized hex literal accepted");
    const std::string document = R"({
      "schema":1, "targets":[{"arch":"test","byte_order":"little","addr_size":32}],
      "extension":{"value":18446744073709551615,"fraction":0.25},
      "backend":{"schema":1,"arch":"test","register_allocation":{},
        "instruction_selection":{},"instruction_scheduling":{},"dependencies":{},
        "instructions":{"ADD":{"unknown":null,"bits":[1,null,0],
          "large":18446744073709551615}}}
    })";
    auto spec = symspec::Spec::from_json(document);
    check(bool(spec), "valid spec rejected");
    domsymas::Builder builder;
    auto result = domsymas::cc::compile_spec(*spec, builder);
    check(result.ok, result.error.c_str());
    check(builder.data().targets.size() == 1, "assembler target lost");
    auto roundtrip = symspec::Spec::from_json(result.spec_json);
    check(roundtrip && roundtrip->json() == spec->json(), "compiler lost backend or extension data");
    auto backend = spec->serialize(symspec::OutputFormat::backend_json);
    domsymas::Builder empty;
    check(!domsymas::cc::compile_document(*backend, empty).ok,
          "backend-only spec accepted as assembler bank");
    check(empty.data().targets.empty(), "backend-only rejection changed builder");
    auto broken = spec->edited([](symspec::Json &json) -> symspec::Status {
        json.as_object().at("backend").as_object().at("instructions").as_object()
            .at("ADD").as_object()["Uses"] = boost::json::object{{"kind","def"},{"def","missing"}};
        return std::monostate{};
    }, symspec::ParseOptions{64 * 1024 * 1024, 256, false});
    check(bool(broken), "unchecked fixture failed");
    check(!domsymas::cc::compile_document(*broken->serialize(), empty).ok,
          "unresolved backend reference accepted");
    check(!domsymas::cc::compile_document("{\"schema\":01}", empty).ok,
          "malformed JSON accepted");
    std::cout << "Spec compiler: assembler mapping and lossless backend preservation passed\n";
}
