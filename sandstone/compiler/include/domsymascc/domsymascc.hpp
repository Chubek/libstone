// DomSymAsCC — the DomSymAs compiler (public C++ API).
//
// DomSymAsCC compiles `.dsymspec` files (a Lua DSL run under the embedded
// DomMune interpreter) into SymAS databanks (`.symas`) via the `domsymas`
// Builder. The DSL accumulates a structured description of an assembler
// target, serializes it to JSON on stdout, and this library maps that
// document onto `domsymas::Builder`.
//
// Layering:
//   .dsymspec (Lua DSL)  ->  JSON document  ->  domsymas::Builder  ->  .symas
//   (DomMune embedded)      (this header)      (domsymas lib)
//
// The JSON schema is documented in `stdspec/README.md` and implemented in
// `src/spec_document.cpp`.
#pragma once

#include <cstdint>
#include <string>

#include "sandstone.hpp"     // domsymas::Builder, domsymas::Databank
#include "Symspec.hpp"

namespace domsymas::cc {

// Current DSL/schema version this compiler understands.
inline constexpr uint32_t kSchemaVersion = 1;

// Result of a compile step.
struct Result {
    bool ok = false;
    std::string error;                 // human-readable diagnostic
    std::string spec_json;             // the JSON document (for --emit-spec)
};

// ---- Pure document -> builder mapping (no DomMune required) ----------------
//
// `document` is the JSON produced by the DSL (the value of `symas.emit()`).
// On success the supplied builder has been populated with all records and
// cross-links; the caller still owns the final `build()`/`save_file()` step.
Result compile_document(const std::string &document_json, domsymas::Builder &builder);

// Compile the assembler records of a validated specification. Backend metadata
// is retained in Result::spec_json; the current SymAS bank cannot encode it.
// Backend-only specs can be queried/exported, but cannot build an assembler bank.
Result compile_spec(const symspec::Spec &spec, domsymas::Builder &builder);

// Load JSON, a .dsymspec source, or a generated backend module via Symspec.
Result compile_file(const symspec::Path &path, domsymas::Builder &builder,
                    const symspec::LoadOptions &options = {});

// Convenience: compile a document straight into an in-memory databank.
Result compile_document_to_bank(const std::string &document_json, domsymas::Databank &out);

} // namespace domsymas::cc
