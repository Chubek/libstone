#include <Flintstone.hpp>
#include <blackstone.hpp>
#include <xbind.h>
#include <filesystem>
#include <iostream>

#ifdef HAVE_Novostone__ActionSemantics
#include <novostone/ActionSemantics/ActionSemantics.hpp>
#include <novostone/Datalog/Datalog.hpp>
#endif
#ifdef HAVE_Moonstone__ExoML
#include <exoml/ExoML.hpp>
#endif
#ifdef HAVE_Moonstone__PlaidRuntime
#include <PlaidRuntime.h>
#endif
#ifdef HAVE_Moonstone__Qmr
#include <qmr/Qmr.hpp>
#endif
#ifdef HAVE_Sandstone__ArchML
#include <ArchML.hpp>
#endif
#ifdef HAVE_Wetstone__wetstone
#include <wetstone/abstract-interp/Interval.hpp>
#endif
#ifdef HAVE_Weighstone__Weighstone
#include <Weighstone/CCINT.hpp>
#endif
#ifdef HAVE_qobjld__qobjld
#include <ld.h>
#include <qBFD.hpp>
#endif
#ifdef HAVE_Tourchstone__dynload
#include <dynload/dynload.h>
#endif
#ifdef HAVE_Tourchstone__dyninvoke
#include <dyninvoke/dyninvoke.h>
#endif

namespace {
void check(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
}

int main(int argc, char** argv) try {
    if (argc == 2) {
        auto directory = std::filesystem::canonical(flintstone::default_kernel_directory());
        const auto prefix = std::filesystem::canonical(argv[1]);
        auto relative = directory.lexically_relative(prefix);
        check(!relative.empty() && *relative.begin() != "..", "did not discover the relocated kernel data");
    }
    blackstone::Runtime kernels;
    kernels.load_directory(flintstone::default_kernel_directory());
    check(kernels.kernels().size() >= 191, "Blackstone catalog is incomplete");
    check(xbind_target_count() > 0, "XBind catalog missing");

    flintstone::Registry registry;
    auto module = flintstone::parse(R"(module m level core {
        func @f(%x:i8)->i8 {
          ^e:
            %a:i8 = arith.mul(%x, -1:i8);
            %b:i8 = arith.add(%a, 0:i8);
            func.return(%b);
        }
    })", "sdk.flint");
    const auto original = module;
    const auto reports = flintstone::optimize(module, registry);
    check(reports.at(0).changed && reports.at(1).changed, "Blackstone and DCE did not run");
    const auto text = flintstone::print(module);
    check(text.find("arith.neg") != std::string::npos && text.find("arith.mul") == std::string::npos &&
          text.find("arith.add") == std::string::npos, "extended kernel did not change Flintstone IR");
    for (int n = -128; n <= 127; ++n)
        check(flintstone::execute(module, registry, "f", {std::int64_t(n)}) ==
              flintstone::execute(original, registry, "f", {std::int64_t(n)}), "optimization changed modular results");

#ifdef HAVE_Novostone__ActionSemantics
    const auto dl = novostone::datalog::parse(".decl Number(integer). Number(42).");
    check(dl.program.evaluate(dl.facts, "Number").rows.size() == 1, "Datalog evaluation failed");
    const auto semantics = novostone::action::parse("language Test { start E; syntax E = N(); rule N = give 42; }");
    check(semantics.name() == "Test" && !generate(semantics, novostone::action::Target::C).empty(),
          "ActionSemantics generation failed");
#endif
#ifdef HAVE_Moonstone__ExoML
    const auto exo = moonstone::exoml::Parser("M ~setconst! Types:String(42);").parse();
    check(exo.commands.at(0).binding == "M", "ExoML parsing failed");
#endif
#ifdef HAVE_Moonstone__PlaidRuntime
    plaid_runtime* runtime{};
    check(plaid_runtime_create(&runtime) == PLAID_OK, "Plaid runtime creation failed");
    int result = 0;
    const auto posted = plaid_runtime_post(runtime, [](void* n) { *static_cast<int*>(n) = 42; }, &result);
    size_t executed = 0;
    const auto ran = plaid_runtime_run(runtime, 2, &executed);
    plaid_runtime_destroy(runtime);
    check(posted == PLAID_OK && ran == PLAID_OK && executed == 1 && result == 42, "Plaid callback didn't run");
#endif
#ifdef HAVE_Moonstone__Qmr
    check(moonstone::qmr::Engine{}.render("{{ 20 + 22 }}") == "42", "QMR rendering failed");
#endif
#ifdef HAVE_Sandstone__ArchML
    check(sandstone::archml::Document::parse("(archml 1 test (target test (bits 64) (endian little)))").name()
          == "test", "ArchML parsing failed");
#endif
#ifdef HAVE_Wetstone__wetstone
    check(wetstone::ai::Interval::point(20).add(wetstone::ai::Interval::point(22)).lo == 42,
          "Wetstone interval transfer failed");
#endif
#ifdef HAVE_Weighstone__Weighstone
    using weighstone::ccint::BitVector;
    check((BitVector(8, 255) + BitVector(8, 43)).unsigned_value() == 42, "Weighstone addition failed");
#endif
#ifdef HAVE_qobjld__qobjld
    auto* linker = qld_create();
    check(linker != nullptr, "Yellowstone linker creation failed");
    qld_destroy(linker);
#endif
#ifdef HAVE_Tourchstone__dynload
    check(*tdyn_version(), "Tourchstone loader version missing");
#endif
#ifdef HAVE_Tourchstone__dyninvoke
    check(*dinv_version(), "Tourchstone invoker version missing");
#endif
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
