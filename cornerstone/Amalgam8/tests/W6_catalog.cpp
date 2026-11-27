#include "../Catalog.hpp"

#include <cassert>
#include <iostream>

int main(int argc, char** argv) {
    assert(argc == 3);
    am8::Syntax use;
    use.span = {"catalog.am8", 0, 0, 4, 2};
    am8::Options options;
    options.include_root = am8::fs::path(argv[1]) / "unused-external-include-root";
    const auto external = am8::resolve_firestone("BigInt", use, options);
    assert(external.name == "firestone:libbignum/BigInt.hpp");
    assert(external.header == "firestone/libbignum/BigInt.hpp");
    assert(external.kind == am8::DependencyKind::external);
    assert(external.context == am8::Context::cpp);
    assert(external.conditions.starts_with("available:"));
    assert(external.libraries == std::vector<std::string>{"Firestone::bignum"});
    assert(external.include_roots == std::vector<std::string>{"."});
    assert(am8::resolve_firestone("firestone/libbignum/BigInt.hpp", use, options).name == external.name);
    assert(am8::resolve_firestone("bignum/BigInt", use, options).name == external.name);
    assert(am8::resolve_firestone("libbignum/BigInt", use, options).name == external.name);

    const auto reject = [&](std::string_view name, std::string_view expected) {
        try {
            (void)am8::resolve_firestone(name, use, options);
        } catch (const am8::Error& error) {
            const std::string_view message(error.what());
            assert(message.starts_with("catalog.am8:4:2: "));
            assert(message.find(expected) != std::string_view::npos);
            return;
        }
        assert(false && "expected catalog error");
    };
    reject("Common.hpp", "ambiguous");
    reject("tests/consumer/private.hpp", "unknown Firestone public header");
    reject("../bignum/BigInt.hpp", "invalid include component");

    options.bundle = true;
    const auto unverified = am8::resolve_firestone("libcompute/CUDA.hpp", use, options);
    assert(unverified.kind == am8::DependencyKind::external);
    assert(unverified.conditions.starts_with("unverified:"));
    assert(unverified.bundled_source.empty());
    options.include_root = argv[1];
    const auto output = [&](std::string_view name, std::string_view file) {
        const auto bundled = am8::resolve_firestone(name, use, options);
        assert(bundled.kind == am8::DependencyKind::bundled);
        assert(bundled.libraries.empty());
        assert(bundled.include_roots.empty());
        assert(bundled.bundled_source.find(am8::packaging::mit_notice) != std::string::npos);
        assert(bundled.bundled_source.find("#pragma once") == std::string::npos);
        assert(am8::resolve_firestone(name, use, options).bundled_source == bundled.bundled_source);
        std::ofstream stream(am8::fs::path(argv[2]) / file, std::ios::binary);
        stream << "#ifdef __cplusplus\n" << bundled.bundled_source << "#endif\n";
        assert(stream);
        return bundled;
    };
    try {
        const auto bigfloat = output("libbignum/BigFloat.hpp", "bundled_float.h");
        assert(bigfloat.requires_names == std::vector<std::string>{external.name});
        assert(bigfloat.bundled_source.find("#include \"BigInt.hpp\"") == std::string::npos);
        assert(bigfloat.bundled_source.find("#include <vector>") != std::string::npos);
        output("BigInt", "bundled_int.h");
        output("Complex128", "bundled_complex.h");
    } catch (const am8::Error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
