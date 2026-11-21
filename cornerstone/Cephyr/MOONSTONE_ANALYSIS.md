# Optional Moonstone analysis in Cephyr

`--moonstone-analyze` is an advisory pass for builds with
`CEPHYR_ENABLE_MOONSTONE=ON` and the Syren, Chisel, Supersonic, and Nerotypical
Sequent targets enabled. It inspects Cephyr's typed frontend MIR after
successfully lowering each function, before serialization and SDK MIR analysis
or rewrites. This applies to C and preprocessed C; `-E` does not perform
lowering and is incompatible with the analysis flag.
It does not change MIR, generate code, create edits, or certify compilation
results. Syren remains Cephyr's existing text/SARIF diagnostic backend.

| Component | Supported input and result | Not covered |
| --- | --- | --- |
| Syren | Existing diagnostics collector renders analysis warnings and notes in text/SARIF with the normal severity policy. | Runtime event recording, diagnostic-rule inference, fix-its, and repair validation are not part of this pass. |
| Chisel | Definite initialization of direct, whole, nonvolatile integer frame slots. Run Chisel's CFG solver and inspect the converged input states. Report a **Heuristic** if a load may lack a prior whole-slot store: structural CFG paths may not execute, and MIR does not encode C lifetimes. | Aliases, address escapes, partial/projected memory, pointer and global memory, calls (including `malloc`/`free`), ownership/borrow, lifetime, GC, and a proof of C memory safety. A function outside the adapter's effect allowlist is **Unknown**, not safe. |
| Nerotypical | Translate and type-check a closed, straight-line MIR function over signed 32/64-bit integers with constants, copy, exact sign extension, and in-range `+`, `-`, `*`. Independently check the MIR operations and compare the evaluated Sequent result. **Verified** means equal return value for that bounded, effect-free projection. | Parameters, general CFG, casts with value changes, overflow, memory, FFI, full C-to-Sequent lowering, Satie typing, and an emitted Sequent artifact. |
| Supersonic | Search a small integer grammar for a cheaper expression with the same return as a verified closed projection. Independently replay the candidate with checked signed arithmetic at the declared result width. **Verified** refers only to this closed MIR return, not to a C edit or sampled input equivalence. | Program repair, open-input equivalence, target optimization, and automatic application. No candidate within the search bound is neither a proof of optimality nor a safety result. |

Only explicit opcode allowlists are accepted. Unknown operations, malformed
operands/CFG, unsupported effects, and resource exhaustion are reported
separately from findings; no partial Chisel finding is published for an
unsupported function. Ordinary C overwrites initialize the slot without
implying any Chisel ownership move or drop. Analysis consumes actual source
origins (or falls back to the function declaration if missing), never invented
source ranges. The adapter emits at most 32 findings plus a suppression note
per invocation. It bounds work by function, CFG blocks, tracked frame slots,
modeled instructions, Chisel transfers, Sequent instructions/fuel, and
synthesis candidates. Heuristic findings are warnings and follow Cephyr's
existing `-Werror` policy; scope and evidence notes do not change compilation
status. There is no runtime or plugin component.

```sh
cephyr --moonstone-analyze -fsyntax-only input.c
cephyr --moonstone-analyze --diagnostics-format=sarif -fsyntax-only input.c
```

Integration cases: a direct uninitialized load, an initialized slot overwritten
normally, a CFG join with a possible uninitialized load, a dead CFG block, a
call and escaped/projection addresses, exact closed arithmetic and a cheaper
synthesized expression, overflow and analysis-budget failures. Exercise text
and SARIF (including a preprocessor line map), the opt-in and disabled-build
behavior, and byte-identical MIR with and without analysis. Run
`ctest --test-dir <cephyr-build> -R cephyr.moonstone-analysis --output-on-failure`.
