#include "../FactIR.hpp"

#include <iostream>

static bool known_bits_proof_inputs()
{
    using cephyr::factir::function_known_bits;
    using cephyr::factir::Bits;
    const ArchonMIRType types[] = {
        ARCHON_MIR_I64, ARCHON_MIR_I64, ARCHON_MIR_I64, ARCHON_MIR_I64,
        ARCHON_MIR_I64, ARCHON_MIR_I64
    };
    const ArchonValue initial = 1, parameter = 2, backedge = 5;
    const ArchonMIRInst entry[] = {{ARCHON_MIR_CONST, initial, ARCHON_MIR_NONE,
        ARCHON_MIR_NONE, 8, nullptr, nullptr, 0}};
    // The backedge OR has the same constant on the first run, but "partial"
    // case with the unknown ABI input can also produce other high/low bits.
    ArchonMIRInst loop[] = {
        {ARCHON_MIR_CONST, 3, ARCHON_MIR_NONE, ARCHON_MIR_NONE, 0, nullptr, nullptr, 0},
        {ARCHON_MIR_OR, 4, 3, initial, 0, nullptr, nullptr, 0},
        {ARCHON_MIR_COPY, backedge, 4, ARCHON_MIR_NONE, 0, nullptr, nullptr, 0},
    };
    ArchonMIRBlock blocks[] = {
        {nullptr, 0, entry, 1, {ARCHON_MIR_BR, ARCHON_MIR_NONE, {1, &initial, 1}, {}}},
        {&parameter, 1, loop, 3, {ARCHON_MIR_CBR, 0, {1, &backedge, 1}, {2, nullptr, 0}}},
        {nullptr, 0, nullptr, 0, {ARCHON_MIR_RETURN, parameter, {}, {}}},
    };
    const ArchonMIRFunction fn{"phi", ARCHON_MIR_I64, types, 6, 1, blocks, 3, nullptr, 0};
    const auto exact = function_known_bits(fn)[parameter];
    loop[1].a = 0;
    const auto partial = function_known_bits(fn)[parameter];
    loop[1].a = 3;
    loop[1].op = ARCHON_MIR_MUL_CHECKED_S;
    const auto unmodeled = function_known_bits(fn)[parameter];
    return exact.all_known(UINT64_MAX) && exact.one == 8 &&
           !partial.all_known(UINT64_MAX) && partial.zero == 0 && partial.one == 8 &&
           unmodeled.zero == 0 && unmodeled.one == 0;
}

int main()
{
    using cephyr::factir::prove_add_constant;
    if (!prove_add_constant(0).unsatisfiable() ||
        !prove_add_constant(1).satisfiable() ||
        prove_add_constant(0, 1).status != satie::SolveStatus::UNKNOWN) {
        std::cerr << "FactIR proof did not distinguish unsat, sat, and a spent search budget\n";
        return 1;
    }
    if (!known_bits_proof_inputs()) {
        std::cerr << "FactIR loop join did not distinguish exact, partial, and unmodeled bits\n";
        return 1;
    }
    if (!cephyr::factir::prove_boolize(~UINT64_C(1)).unsatisfiable() ||
        !cephyr::factir::prove_boolize(~UINT64_C(3)).satisfiable()) {
        std::cerr << "Double zero test is an identity only on canonical boolean inputs\n";
        return 1;
    }
    return 0;
}
