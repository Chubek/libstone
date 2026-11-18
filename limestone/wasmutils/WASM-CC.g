(* Descriptive EBNF of the emitted C90 source, not a C parser input.
   Uppercase leaves stand for emitter templates, not user-supplied tokens. *)
translation_unit = C89_PREAMBLE, entry ;
entry = ENTRY_DECL, "{", (TRAP_BODY | EMPTY_BODY), "}"
      | WORD_DECL, { INTEGER_HELPER }, ENTRY_DECL, "{",
        FIXED_STACK_DECL, RESULT_COUNT_AND_CAPACITY_CHECK,
        { statement }, RESULT_COPY, { RESULT_COPY }, "return", "0", ";", "}" ;
statement = CONSTANT_ASSIGNMENT | SELECT_ASSIGNMENT
          | UNARY_ASSIGNMENT | BINARY_ASSIGNMENT ;

(* Validated drop and nop instructions emit no statements. All other
   operations are as defined in WASM.aml. Runtime i64 operations use two
   32-bit limbs; the result ABI carries both limbs and the type width.
   An unreachable stream has no observable work other than reporting trap. *)
