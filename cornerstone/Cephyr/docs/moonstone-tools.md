# Cephyr's optional Moonstone tools

## Design and boundaries

These tools are opt-in at build time and must be selected explicitly on each
invocation. None is a fallback for native C compilation.

For a standalone build with all five capabilities, configure Cephyr with
`-DCEPHYR_ENABLE_MOONSTONE=ON -DMOONSTONE_BUILD_EXOTRACT=ON
-DMOONSTONE_BUILD_QMR=ON`. ExoML, Plaid and Prolific must also be enabled in
Moonstone (their defaults). ExoML extraction is not required for these tools.
In a LibStone build, Cephyr reuses the selected Moonstone targets.

```sh
cephyr --moonstone-profile=compile.json -fsyntax-only example.c
cephyr --moonstone-exoml bindings.exo Source > bindings.hpp
cephyr --moonstone-exotract vendor.h VENDOR_IMPLEMENTATION > vendor.c
cephyr --moonstone-qmr-compile page.qmr > page.lua
cephyr --moonstone-qmr-check --jobs=4 header.qmr page.qmr footer.qmr
```

- **Prolific** records a bounded compiler event stream per request, using
  lexical spans on the caller thread and source names as supplied by Cephyr.
  The driver closes spans before taking the snapshot, records success or
  normal compilation failure, and publishes Prolific JSON independently of
  compiler diagnostics and code output. The destination must be a file, not
  `-`, and cannot alias an input, configuration file, or code output.
  Reports contain input names, which may be private.
  `complete` describes collection completeness, not compiler success; the
  `cephyr.result` event carries the outcome when successfully recorded.
  No Lua analysis, speculative rewrites, cache, runtime injection, or proof
  is involved.
- **ExoML** takes a trusted declarative StaticFFI file and the name of its string
  output. A fresh interpreter registers only the in-memory StaticFFI/Types
  services. Hatch commands are rejected; declared C functions are not called.
  Generated bindings, ABI checks, and probes are text for a later target
  toolchain; the bridge does not infer or validate any ABI layout.
- **Exotract** takes an existing header path and explicit implementation
  macro names. It calls `Engine::implementation_source()` to generate one C11
  implementation translation unit. It does not invoke Exotract preprocessing,
  inspection, typed discovery, or any subprocess; it does not imply semantic
  contracts or verified bindings.
- **QMR** translates a template into Lua source using the native QMR compiler.
  A batch command checks translation only (not Lua syntax or evaluation).
  Neither command executes Lua, evaluates user scripts, or resolves includes.
- **Plaid** runs only the independent, in-memory QMR batch translations when
  `--jobs=N` requests more than one worker. File reads, diagnostics and output
  stay on the caller thread. Batch check accepts up to 128 files and defaults
  to one job (no worker pool); the maximum is eight jobs. At most eight jobs
  are submitted in a wave; each owns its input and no work escapes the
  invocation. Waiting and joining block the caller OS thread; callbacks may
  start on any worker but do not migrate during execution. There is no forced
  cancellation: accepted jobs finish and are joined even if another job fails.
  No C compilation, SDK callback, target program, or concurrent compile
  request is scheduled by this pool.

Tool failure reports a diagnostic and returns no artifact. Unsupported tools
and requests for a disabled Plaid pool fail explicitly rather than silently
substituting another implementation. Diagnostics retain source locations from
the linked parser or translator, and batch diagnostics follow input order.
The tool flags must be the first argument and do not take compiler options.
Their input paths must name existing regular files. The four tool modes write
to compile-request stdout, or CLI stdout; they do not run SDK hooks.

## Regression coverage

`cephyr.moonstone-tools` checks successful and failed profile collection,
including span pairing, source identity, outcome, collection loss, file
collisions, and unchanged MIR. It compiles generated ExoML bindings against
a real header and rejects signature mismatch, hatch, and missing symbols.
It compiles and runs a fixture's generated Exotract implementation TU and
rejects duplicate or malformed macro names. It checks generated Lua source
and QMR template locations, ordered batch failures, and bounded inputs. With
Plaid enabled, the test compares results from several worker counts to serial
execution. Disabled builds exercise explicit rejection of unavailable tools.
