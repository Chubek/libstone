/* Archon runtime native backend. See MachineIR.md for operation contracts. */
#ifndef ARCHON_BACKEND_H
#define ARCHON_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHON_BACKEND_ABI_VERSION 1u
#define ARCHON_MIR_SCHEMA_VERSION 1u
#define ARCHON_MIR_TEXT_MAX_BYTES (4u * 1024u * 1024u)
#define ARCHON_MIR_NONE UINT32_MAX
typedef uint32_t ArchonValue;
typedef enum {
    ARCHON_MIR_VOID = 0, ARCHON_MIR_I64, ARCHON_MIR_PTR, ARCHON_MIR_FUNC_PTR
} ArchonMIRType;
typedef enum {
    ARCHON_MIR_CONST = 1, ARCHON_MIR_COPY,
    ARCHON_MIR_ADD, ARCHON_MIR_SUB, ARCHON_MIR_MUL,
    ARCHON_MIR_DIV_S, ARCHON_MIR_REM_S, ARCHON_MIR_DIV_U, ARCHON_MIR_REM_U,
    ARCHON_MIR_AND, ARCHON_MIR_OR, ARCHON_MIR_XOR,
    ARCHON_MIR_SHL, ARCHON_MIR_LSHR, ARCHON_MIR_ASHR,
    ARCHON_MIR_EQ, ARCHON_MIR_SLT, ARCHON_MIR_ULT,
    ARCHON_MIR_ADD_CHECKED_S, ARCHON_MIR_SUB_CHECKED_S, ARCHON_MIR_DIV_CHECKED_S,
    ARCHON_MIR_PTR_ADD, ARCHON_MIR_LOAD, ARCHON_MIR_STORE,
    ARCHON_MIR_FRAME_ADDR, ARCHON_MIR_GLOBAL_ADDR, ARCHON_MIR_CALL,
    ARCHON_MIR_FUNC_ADDR, ARCHON_MIR_CALL_INDIRECT,
    ARCHON_MIR_TRUNC, ARCHON_MIR_ZEXT, ARCHON_MIR_SEXT,
    ARCHON_MIR_MUL_CHECKED_S,
    /* Appended to preserve every MachineIR v1 opcode value above. */
    ARCHON_MIR_LOAD_S8, ARCHON_MIR_LOAD_U8,
    ARCHON_MIR_LOAD_S16, ARCHON_MIR_LOAD_U16,
    ARCHON_MIR_LOAD_S32, ARCHON_MIR_LOAD_U32,
    ARCHON_MIR_LOAD_PTR, ARCHON_MIR_LOAD_FUNC_PTR,
    ARCHON_MIR_STORE8, ARCHON_MIR_STORE16, ARCHON_MIR_STORE32,
    ARCHON_MIR_STORE_PTR, ARCHON_MIR_STORE_FUNC_PTR,
    ARCHON_MIR_PTR_TO_INT, ARCHON_MIR_INT_TO_PTR,
    ARCHON_MIR_FUNC_PTR_TO_INT, ARCHON_MIR_INT_TO_FUNC_PTR
} ArchonMIROpcode;
typedef struct {
    ArchonMIROpcode op;
    ArchonValue dst, a, b;
    uint64_t imm;
    const char *symbol;
    const ArchonValue *args;
    size_t arg_count;
} ArchonMIRInst;
typedef struct {
    uint32_t target;
    const ArchonValue *args;
    size_t arg_count;
} ArchonMIREdge;
typedef enum {
    ARCHON_MIR_RETURN = 1, ARCHON_MIR_BR, ARCHON_MIR_CBR, ARCHON_MIR_TRAP
} ArchonMIRTermKind;
typedef struct {
    ArchonMIRTermKind kind;
    ArchonValue value;
    ArchonMIREdge yes, no;
} ArchonMIRTerm;
typedef struct {
    const ArchonValue *params;
    size_t param_count;
    const ArchonMIRInst *insts;
    size_t inst_count;
    ArchonMIRTerm term;
} ArchonMIRBlock;
typedef struct {
    uint32_t size, alignment;
} ArchonMIRLocal;
typedef struct {
    const char *name;
    ArchonMIRType result;
    const ArchonMIRType *types;
    size_t value_count, param_count; /* Values [0, param_count) are ABI parameters. */
    const ArchonMIRBlock *blocks;
    size_t block_count;
    const ArchonMIRLocal *locals;
    size_t local_count;
} ArchonMIRFunction;
typedef struct {
    const char *name;
    ArchonMIRType result;
    const ArchonMIRType *params;
    size_t param_count;
} ArchonMIRImport;
typedef struct {
    const char *name;
    const uint64_t *words; /* NULL means zero initialization (or external). */
    size_t word_count;    /* Zero declares an external data symbol. */
} ArchonMIRGlobal;
typedef struct {
    unsigned version; /* ARCHON_MIR_SCHEMA_VERSION. */
    const ArchonMIRFunction *functions;
    size_t function_count;
    const ArchonMIRImport *imports;
    size_t import_count;
    const ArchonMIRGlobal *globals;
    size_t global_count;
} ArchonMIRModule;

typedef enum {
    ARCHON_BACKEND_OK = 0, ARCHON_BACKEND_INVALID, ARCHON_BACKEND_UNSUPPORTED,
    ARCHON_BACKEND_LIMIT, ARCHON_BACKEND_NOMEM, ARCHON_BACKEND_CANCELLED,
    ARCHON_BACKEND_INTERNAL
} ArchonBackendStatus;
typedef enum {
    ARCHON_BACKEND_INPUT = 0, ARCHON_BACKEND_VERIFY, ARCHON_BACKEND_SELECT,
    ARCHON_BACKEND_ALLOCATE, ARCHON_BACKEND_EMIT, ARCHON_BACKEND_SCHEDULE
} ArchonBackendStage;
enum {
    ARCHON_BACKEND_SCHEDULE_PURE = 1u,
    ARCHON_BACKEND_PHYSICAL_RECORDS = 2u,
    ARCHON_BACKEND_MACHINE_CODE = 4u,
    /* Assembly-only: use GAS PIC address materialization. */
    ARCHON_BACKEND_PIC = 8u
};
typedef struct {
    ArchonBackendStatus status;
    ArchonBackendStage stage;
    size_t function, block, instruction; /* SIZE_MAX if unavailable. */
    char message[192];
} ArchonBackendDiagnostic;
typedef struct {
    size_t max_output_bytes; /* 0 selects the default. */
    int (*cancelled)(void *user);
    void *user;
} ArchonBackendOptions;
typedef struct {
    char *data;
    size_t size;
} ArchonBackendArtifact;

/* Borrowed immutable input/options for this call. Output is owned by the caller
 * and must be empty on entry; on failure no artifact is published. */
ArchonBackendStatus archon_backend_compile(const ArchonMIRModule *module,
    const ArchonBackendOptions *options, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic);
/* Flags zero selects the existing non-PIC assembly. Output modes are mutually
 * exclusive. PIC is valid only for assembly output.
 * MACHINE_CODE returns raw position-independent instruction bytes for the
 * generated RV64I or RV64IM target, for one function (entry at byte zero, no
 * imports or globals). Use size, not strlen; the library does not execute it. */
ArchonBackendStatus archon_backend_compile_ex(const ArchonMIRModule *module,
    const ArchonBackendOptions *options, unsigned flags, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic);
ArchonBackendStatus archon_backend_compile_physical(const ArchonMIRModule *module,
    const ArchonBackendOptions *options, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic);
/* Decode a length-delimited canonical text stream as data, not as C source. */
ArchonBackendStatus archon_backend_compile_text(const char *text, size_t size,
    const ArchonBackendOptions *options, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic);
ArchonBackendStatus archon_backend_compile_text_ex(const char *text, size_t size,
    const ArchonBackendOptions *options, unsigned flags, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic);
void archon_backend_artifact_destroy(ArchonBackendArtifact *artifact);
const char *archon_backend_target(void);

#ifdef __cplusplus
}
#endif
#endif
