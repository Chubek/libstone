/* Additional MIR and API boundary cases for the end-to-end test. */
#include "Backend.h"

#include <stdio.h>
#include <string.h>

#define LEN(xs) (sizeof(xs) / sizeof((xs)[0]))
#define N ARCHON_MIR_NONE
#define I ARCHON_MIR_I64
#define P ARCHON_MIR_PTR
#define V ARCHON_MIR_VOID

#define BIN(op, d, x, y) {ARCHON_MIR_##op, d, x, y, 0, NULL, NULL, 0}
#define CONST(d, k) {ARCHON_MIR_CONST, d, N, N, k, NULL, NULL, 0}
#define MEM(op, d, x, y, off) {ARCHON_MIR_##op, d, x, y, off, NULL, NULL, 0}
#define ADDR(d, s) {ARCHON_MIR_GLOBAL_ADDR, d, N, N, 0, s, NULL, 0}
#define CALL(d, s, argv) {ARCHON_MIR_CALL, d, N, N, 0, s, argv, LEN(argv)}
#define RET(x) {.kind = ARCHON_MIR_RETURN, .value = x}
#define BR(to, xs) {.kind = ARCHON_MIR_BR, .value = N, .yes = {to, xs, LEN(xs)}}
#define CBR(cond, t, f) {.kind = ARCHON_MIR_CBR, .value = cond, .yes = {.target = t}, \
                        .no = {.target = f}}
#define FN(s, rt, ty, n, bb) {s, rt, ty, LEN(ty), n, bb, LEN(bb), NULL, 0}

/* The backedge swaps two block parameters in parallel, not sequentially. */
static const ArchonMIRType rotate_types[] = {I, I, I, I, I, I, I, I, I, I, I};
static const ArchonValue rotate_entry[] = {0, 1, 2};
static const ArchonValue rotate_params[] = {3, 4, 5};
static const ArchonValue rotate_next[] = {7, 5, 4};
static const ArchonMIRInst rotate_step[] = {CONST(6, 1), BIN(SUB, 7, 3, 6)};
static const ArchonMIRInst rotate_result[] = {
    CONST(8, 100), BIN(MUL, 9, 4, 8), BIN(ADD, 10, 9, 5),
};
static const ArchonMIRBlock rotate_blocks[] = {
    {.term = BR(1, rotate_entry)},
    {.params = rotate_params, .param_count = LEN(rotate_params), .term = CBR(3, 2, 3)},
    {.insts = rotate_step, .inst_count = LEN(rotate_step), .term = BR(1, rotate_next)},
    {.insts = rotate_result, .inst_count = LEN(rotate_result), .term = RET(10)},
};

static const ArchonMIRType gcd_types[] = {I, I, I, I, I};
static const ArchonValue gcd_input[] = {0, 1}, gcd_params[] = {2, 3}, gcd_next[] = {3, 4};
static const ArchonMIRInst gcd_step[] = {BIN(REM_U, 4, 2, 3)};
static const ArchonMIRBlock gcd_blocks[] = {
    {.term = BR(1, gcd_input)},
    {.params = gcd_params, .param_count = LEN(gcd_params), .term = CBR(3, 2, 3)},
    {.insts = gcd_step, .inst_count = LEN(gcd_step), .term = BR(1, gcd_next)},
    {.term = RET(2)},
};

static const ArchonMIRType sum_types[] = {P, I, I, I, I, P, I, I, I, P, I, I};
static const ArchonValue sum_input[] = {0, 1, 2}, sum_params[] = {5, 6, 7},
    sum_next[] = {9, 10, 11};
static const ArchonMIRInst sum_init[] = {CONST(2, 0), CONST(3, 8), CONST(4, 1)};
static const ArchonMIRInst sum_step[] = {
    MEM(LOAD, 8, 5, N, 0), BIN(PTR_ADD, 9, 5, 3), BIN(SUB, 10, 6, 4),
    BIN(ADD, 11, 7, 8),
};
static const ArchonMIRBlock sum_blocks[] = {
    {.insts = sum_init, .inst_count = LEN(sum_init), .term = BR(1, sum_input)},
    {.params = sum_params, .param_count = LEN(sum_params), .term = CBR(6, 2, 3)},
    {.insts = sum_step, .inst_count = LEN(sum_step), .term = BR(1, sum_next)},
    {.term = RET(7)},
};

static const ArchonMIRType factorial_types[] = {I, I, I, I, I};
static const ArchonValue recursive_args[] = {2};
static const ArchonMIRInst one[] = {CONST(1, 1)};
static const ArchonMIRInst recursive[] = {
    BIN(SUB, 2, 0, 1), CALL(3, "factorial", recursive_args), BIN(MUL, 4, 0, 3),
};
static const ArchonMIRBlock factorial_blocks[] = {
    {.insts = one, .inst_count = LEN(one), .term = CBR(0, 1, 2)},
    {.insts = recursive, .inst_count = LEN(recursive), .term = RET(4)},
    {.term = RET(1)},
};

/* First test stack inputs from C, then shuffle stack arguments on a call to C. */
static const ArchonMIRType ten_params[] = {I, I, I, I, I, I, I, I, I, I};
static const ArchonMIRType ten_types[] = {I, I, I, I, I, I, I, I, I, I, I};
static const ArchonValue shuffled[] = {8, 1, 2, 3, 4, 5, 6, 9, 7, 0};
static const ArchonMIRInst difference[] = {BIN(SUB, 10, 8, 9)};
static const ArchonMIRInst shuffled_call[] = {CALL(10, "native_ten", shuffled)};
static const ArchonMIRBlock stack_in_blocks[] = {
    {.insts = difference, .inst_count = LEN(difference), .term = RET(10)},
};
static const ArchonMIRBlock stack_out_blocks[] = {
    {.insts = shuffled_call, .inst_count = LEN(shuffled_call), .term = RET(10)},
};

static const ArchonMIRType memory_types[] = {P, I, P, I, I, P, I, P, I, P, I};
/* The accessed local is deliberately outside signed-12-bit SP reach. */
static const ArchonMIRLocal locals[] = {{4096, 16}, {16, 16}};
static const uint64_t initial_words[] = {UINT64_C(0x123456789abcdef0), UINT64_C(29)};
static const ArchonMIRGlobal globals[] = {
    {"defined_words", initial_words, LEN(initial_words)},
    {"zero_words", NULL, 1},
    {"native_words", NULL, 0},
};
static const ArchonMIRInst memory[] = {
    MEM(FRAME_ADDR, 2, N, N, 1),
    MEM(STORE, N, 2, 1, 8),
    MEM(LOAD, 3, 2, N, 8),
    CONST(4, 8),
    BIN(PTR_ADD, 5, 0, 4),
    MEM(STORE, N, 5, 3, UINT64_C(0) - 8),
    ADDR(7, "defined_words"),
    MEM(LOAD, 6, 7, N, 8),
    ADDR(9, "native_words"),
    MEM(LOAD, 8, 9, N, 0),
    BIN(ADD, 10, 6, 8),
};
static const ArchonMIRBlock memory_blocks[] = {
    {.insts = memory, .inst_count = LEN(memory), .term = RET(10)},
};
static const ArchonMIRType ptr_types[] = {P};
static const ArchonMIRInst global_addr[] = {ADDR(0, "zero_words")};
static const ArchonMIRBlock addr_blocks[] = {
    {.insts = global_addr, .inst_count = LEN(global_addr), .term = RET(0)},
};
static const ArchonMIRType void_types[] = {P, I};
static const ArchonMIRInst void_store[] = {MEM(STORE, N, 0, 1, 0)};
static const ArchonMIRBlock void_blocks[] = {
    {.insts = void_store, .inst_count = LEN(void_store), .term = RET(N)},
};

static const ArchonMIRType arith_types[] = {I, I, I};
#define ARITH(s, op) \
    static const ArchonMIRInst s##_insts[] = {BIN(op, 2, 0, 1)}; \
    static const ArchonMIRBlock s##_blocks[] = { \
        {.insts = s##_insts, .inst_count = LEN(s##_insts), .term = RET(2)}, \
    }
ARITH(checked_add, ADD_CHECKED_S);
ARITH(wrapping_add, ADD);
ARITH(checked_sub, SUB_CHECKED_S);
ARITH(checked_div, DIV_CHECKED_S);
ARITH(wrapping_div, DIV_S);
ARITH(wrapping_rem, REM_S);
ARITH(masked_shift, SHL);

/* No local objects, call frames or phi copies: keeping 32 independent values
 * alive until the reduction gives unavoidable register pressure. */
enum { PRESSURE = 32, PRESSURE_INSTS = 3 * PRESSURE - 1 };

static int reject(const char *test, const ArchonMIRModule *module,
    const ArchonBackendOptions *options, ArchonBackendStatus expected,
    ArchonBackendStage stage, size_t fn, size_t block, size_t inst)
{
    ArchonBackendArtifact artifact = {0};
    ArchonBackendDiagnostic diagnostic = {0};
    ArchonBackendStatus status = archon_backend_compile(module, options, &artifact, &diagnostic);
    /* The module-wide output budget is not attributed to a specific MIR node. */
    int location_ok = expected == ARCHON_BACKEND_LIMIT ||
        (diagnostic.function == fn && diagnostic.block == block &&
         diagnostic.instruction == inst);
    if (status == expected && diagnostic.status == expected && diagnostic.stage == stage &&
            location_ok && diagnostic.message[0] &&
            memchr(diagnostic.message, '\0', sizeof(diagnostic.message)) != NULL &&
            artifact.data == NULL && artifact.size == 0)
        return 1;
    fprintf(stderr, "%s: status %d/%d, stage %d, location %zu:%zu:%zu\n", test,
        (int)status, (int)diagnostic.status, (int)diagnostic.stage,
        diagnostic.function, diagnostic.block, diagnostic.instruction);
    archon_backend_artifact_destroy(&artifact);
    return 0;
}

static int cancelled(void *state)
{
    int *called = state;
    ++*called;
    return 1;
}

static int negative_cases(const ArchonMIRModule *valid)
{
    const size_t na = SIZE_MAX;
    ArchonMIRModule bad = *valid;
    ArchonMIRBlock block = checked_add_blocks[0];
    ArchonMIRInst ins = checked_add_insts[0];
    ArchonMIRFunction fn = FN("bad", I, arith_types, 2, checked_add_blocks);
    ArchonMIRBlock bad_edge_blocks[LEN(rotate_blocks)];
    const ArchonMIRType wrong_params[] = {P, I, I, I, I, I, I, I, I, I};
    const ArchonMIRImport wrong_import = {"native_ten", I, wrong_params, LEN(wrong_params)};
    int calls = 0;
    const ArchonBackendOptions cancel = {.cancelled = cancelled, .user = &calls};
    const ArchonBackendOptions limit = {.max_output_bytes = 1};

    bad.version++;
    if (!reject("version", &bad, NULL, ARCHON_BACKEND_UNSUPPORTED,
            ARCHON_BACKEND_VERIFY, na, na, na))
        return 0;
    bad = *valid;
    bad.functions = NULL;
    if (!reject("missing functions", &bad, NULL, ARCHON_BACKEND_INVALID,
            ARCHON_BACKEND_VERIFY, na, na, na))
        return 0;

    bad.functions = &fn;
    bad.function_count = 1;
    fn.blocks = &block;
    block.insts = &ins;
    ins.a = 3;
    if (!reject("out of range SSA ID", &bad, NULL, ARCHON_BACKEND_INVALID,
            ARCHON_BACKEND_VERIFY, 0, 0, 0))
        return 0;
    ins.a = 2;
    if (!reject("value does not dominate its use", &bad, NULL, ARCHON_BACKEND_INVALID,
            ARCHON_BACKEND_VERIFY, 0, 0, 0))
        return 0;
    ins.a = 0;
    ins.op = (ArchonMIROpcode)999;
    if (!reject("unsupported operation", &bad, NULL, ARCHON_BACKEND_UNSUPPORTED,
            ARCHON_BACKEND_VERIFY, 0, 0, 0))
        return 0;

    fn = (ArchonMIRFunction)FN("bad_edge", I, rotate_types, 3, rotate_blocks);
    memcpy(bad_edge_blocks, rotate_blocks, sizeof(bad_edge_blocks));
    fn.blocks = bad_edge_blocks;
    bad_edge_blocks[0].term.yes.arg_count = 2;
    if (!reject("wrong edge arity", &bad, NULL, ARCHON_BACKEND_INVALID,
            ARCHON_BACKEND_VERIFY, 0, 0, na))
        return 0;

    fn = (ArchonMIRFunction)FN("bad_call", I, ten_types, 10, stack_out_blocks);
    bad.imports = &wrong_import;
    if (!reject("wrong call signature", &bad, NULL, ARCHON_BACKEND_INVALID,
            ARCHON_BACKEND_VERIFY, 0, 0, 0))
        return 0;

    return reject("cancelled", valid, &cancel, ARCHON_BACKEND_CANCELLED,
               ARCHON_BACKEND_INPUT, na, na, na) && calls == 1 &&
           reject("output budget", valid, &limit, ARCHON_BACKEND_LIMIT,
               ARCHON_BACKEND_EMIT, na, na, na);
}

int main(int argc, char **argv)
{
    ArchonMIRInst pressure[PRESSURE_INSTS];
    ArchonMIRType pressure_types[PRESSURE_INSTS + 1];
    const ArchonMIRBlock pressure_blocks[] = {
        {.insts = pressure, .inst_count = LEN(pressure), .term = RET(PRESSURE_INSTS)},
    };
    const ArchonMIRFunction functions[] = {
        FN("rotate_pair", I, rotate_types, 3, rotate_blocks),
        FN("compute_gcd", I, gcd_types, 2, gcd_blocks),
        FN("sum_array", I, sum_types, 2, sum_blocks),
        FN("factorial", I, factorial_types, 1, factorial_blocks),
        FN("stack_in", I, ten_types, 10, stack_in_blocks),
        FN("stack_out", I, ten_types, 10, stack_out_blocks),
        {"memory_roundtrip", I, memory_types, LEN(memory_types), 2,
            memory_blocks, LEN(memory_blocks), locals, LEN(locals)},
        FN("zero_word_address", P, ptr_types, 0, addr_blocks),
        FN("store_word", V, void_types, 2, void_blocks),
        FN("checked_add", I, arith_types, 2, checked_add_blocks),
        FN("wrapping_add", I, arith_types, 2, wrapping_add_blocks),
        FN("checked_sub", I, arith_types, 2, checked_sub_blocks),
        FN("checked_div", I, arith_types, 2, checked_div_blocks),
        FN("wrapping_div", I, arith_types, 2, wrapping_div_blocks),
        FN("wrapping_rem", I, arith_types, 2, wrapping_rem_blocks),
        FN("masked_shift", I, arith_types, 2, masked_shift_blocks),
        FN("spill_pressure", I, pressure_types, 1, pressure_blocks),
    };
    const ArchonMIRImport imports[] = {{"native_ten", I, ten_params, LEN(ten_params)}};
    const ArchonMIRModule module = {ARCHON_MIR_SCHEMA_VERSION,
        functions, LEN(functions), imports, LEN(imports), globals, LEN(globals)};
    ArchonBackendArtifact assembly = {0};
    ArchonBackendDiagnostic diagnostic = {0};
    ArchonValue i, acc;
    FILE *out;
    int ok = 1;

    if (argc != 2)
        return 2;
    for (i = 0; i < LEN(pressure_types); ++i)
        pressure_types[i] = I;
    for (i = 0; i < PRESSURE; ++i) {
        ArchonValue k = i * 2 + 1;
        pressure[k - 1] = (ArchonMIRInst)CONST(k, i + 1);
        pressure[k] = (ArchonMIRInst)BIN(ADD, k + 1, 0, k);
    }
    for (i = 1, acc = 2; i < PRESSURE; ++i) {
        ArchonValue k = PRESSURE * 2 + i;
        pressure[k - 1] = (ArchonMIRInst)BIN(ADD, k, acc, i * 2 + 2);
        acc = k;
    }

    if (archon_backend_compile(&module, NULL, &assembly, &diagnostic)
            != ARCHON_BACKEND_OK) {
        fprintf(stderr, "initial compile: %s (%zu:%zu:%zu)\n", diagnostic.message,
            diagnostic.function, diagnostic.block, diagnostic.instruction);
        return 1;
    }
    /* Failed requests must neither publish an artifact nor corrupt a live one. */
    if (!negative_cases(&module))
        ok = 0;
    for (i = 0; ok && i < 3; ++i) {
        ArchonBackendArtifact repeated = {0};
        if (archon_backend_compile(&module, NULL, &repeated, &diagnostic)
                != ARCHON_BACKEND_OK ||
                repeated.size != assembly.size ||
                memcmp(repeated.data, assembly.data, assembly.size) != 0) {
            fprintf(stderr, "repeated compile differs: %s\n", diagnostic.message);
            ok = 0;
        }
        archon_backend_artifact_destroy(&repeated);
    }

    if (ok) {
        out = fopen(argv[1], "wb");
        if (out == NULL) {
            perror(argv[1]);
            ok = 0;
        } else {
            ok = fwrite(assembly.data, 1, assembly.size, out) == assembly.size;
            if (fclose(out) != 0)
                ok = 0;
        }
    }
    archon_backend_artifact_destroy(&assembly);
    return !ok;
}
