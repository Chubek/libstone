#include "Amalgam8Base.hpp"

#include <fstream>
#include <iostream>

namespace {
void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

void write(const am8::fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    out.close();
    require(static_cast<bool>(out), "could not write generated fixture");
}
} // namespace

int main(int argc, char** argv) try {
    using namespace am8;
    if (argc != 4) return 64;
    const fs::path examples(argv[1]), out(argv[2]);
    auto api = compile_file(examples / "NativePipeline.am8");
    require(api.dependencies.size() == 3, "expected three native dependencies");
    const auto& xbind = api.dependencies[0];
    require(xbind.name == "tourchstone:xbind" && xbind.header == "tourchstone/xbind/xbind.h" &&
            xbind.context == Context::c && xbind.kind == DependencyKind::external &&
            xbind.libraries == std::vector<std::string>{"XBind::xbind"} &&
            xbind.include_roots == std::vector<std::string>{"."}, "bad XBind dependency metadata");
    const auto& core = api.dependencies[1];
    require(core.name == "moonstone:core" && core.context == Context::cpp &&
            core.kind == DependencyKind::external && core.libraries.empty(), "bad Moonstone dependency metadata");
    require(api.dependencies[2].header == "firestone/libbignum/BigInt.hpp", "missing Firestone dependency");
    write(out / "NativePipeline.hpp", api.source);

    Options options;
    options.mode = Mode::cli;
    auto cli = compile_file(examples / "NativePipeline.am8", options);
    write(out / "NativePipeline.cpp", cli.source);

    options.bundle = true;
    options.include_root = argv[3];
    auto mixed = compile_file(examples / "NativePipeline.am8", options);
    require(mixed.dependencies.size() == 3 &&
            mixed.dependencies[0].kind == DependencyKind::external &&
            mixed.dependencies[1].kind == DependencyKind::external &&
            mixed.dependencies[2].kind == DependencyKind::bundled,
            "cross-family bundling inlined a native dependency or left BigInt external");
    write(out / "NativePipelineBundled.cpp", mixed.source);

    const auto c = compile_source(R"AM8(
        (library NativeC
          (use tourchstone xbind/xbind.h)
          (use tourchstone:xbind)
          (extern int am8_xbind_has_target (((ptr (const char)) name)))
          (fn int has_target (((ptr (const char)) name))
            (block (return (call am8_xbind_has_target name)))))
    )AM8", "W7-C.am8");
    require(c.dependencies.size() == 1 && c.dependencies[0].name == xbind.name,
            "legacy/qualified XBind paths were not deduplicated");
    write(out / "NativeC.h", c.source);

    const auto legacy = compile_source(R"AM8(
        (library Legacy
          (use tourchstone dyninvoke/dyninvoke.hpp)
          (use moonstone arbitrary/custom.hh)
          (use moonstone core/SExpr.hpp)
          (use moonstone:core)
          (use moonstone chisel/Chisel.hpp)
          (use moonstone chisel/include/chisel/Chisel.hpp)
          (use moonstone:chisel))
    )AM8", "W7-Legacy.am8");
    require(legacy.dependencies.size() == 4, "legacy C++ imports should resolve and deduplicate");
    require(legacy.dependencies[0].header == "tourchstone/dyninvoke/dyninvoke.hpp" &&
            legacy.dependencies[1].header == "moonstone/arbitrary/custom.hh" &&
            legacy.dependencies[3].header == "moonstone/chisel/include/chisel/Chisel.hpp" &&
            legacy.dependencies[0].context == Context::cpp &&
            legacy.dependencies[1].context == Context::cpp,
            "legacy family-relative headers or C++ context changed");

    bool rejected = false;
    try {
        (void)compile_file(examples / "OptionalTarget.am8");
    } catch (const Error& e) {
        const std::string_view text(e.what());
        rejected = text.find("OptionalTarget.am8:2:") != text.npos &&
                   text.find("tourchstone:dyninvoke") != text.npos &&
                   text.find("TOURCHSTONE_BUILD_DYNINVOKE=ON") != text.npos &&
                   text.find("Tourchstone::dyninvoke") != text.npos &&
                   text.find("qBFD") != text.npos;
    }
    require(rejected, "missing actionable source-located unavailable-target error");
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
