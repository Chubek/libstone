#include <cassert>
#include <chisel/Regions.hpp>
#include <core/SExpr.hpp>
#include <core/SafetyEffects.hpp>
#include <exoml/StaticFFI.hpp>
#include <supersonic/CEGIS.hpp>
#include <syren/Repair.hpp>
using namespace moonstone;
int main() {
    chisel::RegionSolver regions;
    regions.outlives(1, 2);
    regions.outlives(2, 3);
    regions.solve();
    assert(regions.proves_outlives(1, 3));
    auto p = sexpr::Parser("(f (g x) x)").one(), q = sexpr::Parser("(f ?a ?b)").one();
    assert(sexpr::match(q, p));
    syren::RepairCandidate candidate{
        "change", "edit", {{0, 1, "x"}}, 1, syren::RepairStatus::Candidate, {}};
    auto verified = syren::verify_repair(candidate, "a", [](std::string_view s) {
        return ProofResult{s == "x" ? Status::Proven : Status::Refuted, "verified", {}, 0};
    });
    assert(verified.status == syren::RepairStatus::Validated);
    exoml::BindingModule binding;
    binding.name = "ffi";
    binding.functions.push_back({"add", "int", {{"a", "int"}, {"b", "int"}}, {}, false});
    assert(exoml::validate_binding(binding).empty());
    supersonic::Problem synth;
    synth.grammar.variables = {"x"};
    synth.grammar.constants = {1};
    synth.examples = {{{{"x", 3}}, 4}};
    auto result = supersonic::cegis(
        synth, [](const supersonic::Expr &expr) -> std::optional<supersonic::Example> {
            for (std::int64_t x = -5; x <= 5; ++x) {
                auto y = supersonic::evaluate(expr, {{"x", x}});
                if (!y || *y != x + 1)
                    return supersonic::Example{{{"x", x}}, x + 1};
            }
            return std::nullopt;
        });
    assert(!result.fully_verified && result.solution.program &&
           result.proof.status == Status::Unknown);
}
