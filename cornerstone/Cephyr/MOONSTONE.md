# Moonstone integration design

Cephyr's baseline compiler and v1 embedding ABI do not require Moonstone's
compiled libraries. The optional `CEPHYR_ENABLE_MOONSTONE` build links supported
Moonstone capabilities privately. It keeps the C, C++, and Python request
layouts unchanged. A LibStone root build reuses its Moonstone targets; a
standalone Cephyr build can build those targets from the same checkout.

```sh
cmake -S cornerstone/Cephyr -B build-cephyr \
  -DCEPHYR_ENABLE_MOONSTONE=ON \
  -DMOONSTONE_BUILD_EXOTRACT=ON -DMOONSTONE_BUILD_QMR=ON
cmake --build build-cephyr
```

The Exotract and QMR switches are needed for those components; Moonstone
builds them only on request. Other disabled components are not silently
substituted. These commands work through Cephyr's normal C, C++, and Python
embedding argv as well, subject to the same trust requirements.

## Commands

| Use | Component | Boundary |
| --- | --- | --- |
| `--moonstone-analyze` with a C compile command | Syren, Chisel, Nerotypical, Supersonic | [Advisory MIR inspection](MOONSTONE_ANALYSIS.md) on modeled operations. No safety certification or automatic edits. |
| `--moonstone-profile=FILE` with a compile command | Prolific | Phase timings and compilation outcome in Prolific JSON. No source bodies or speculative transformations. |
| `--moonstone-exoml FILE SYMBOL` | ExoML | Generate the named StaticFFI C++ binding source with hatch commands disabled. Compile the generated bindings against the actual target declarations to validate signatures. |
| `--moonstone-exotract HEADER MACRO...` | Exotract | Generate an implementation translation unit for a C header-only package. The result is not a reviewed semantic contract or a safe binding. |
| `--moonstone-qmr-compile FILE` | QMR | Translate a template to Lua source without rendering or executing Lua. |
| `--moonstone-qmr-check [--jobs=N] FILE...` | QMR and optionally Plaid | Translate a bounded batch of templates, optionally on a Plaid host worker pool, retaining reporting order. No template execution or C11 threads. |

The four standalone tools must be the first argument; their output is written
to stdout (or the embedding result) and they do not invoke compiler SDK hooks.
A profile instead attaches to a compile command and cannot share a destination
with compiler stdout, an explicit input, the configuration, or a compiler
output artifact. Ordinary diagnosed compile failures can still produce a
profile; its success marker is separate from Prolific's collection-completeness
field. If profile publication fails after a compiler output is written, Cephyr
reports failure without rolling back that earlier artifact.
See [tool design and constraints](docs/moonstone-tools.md) for more details.

The commands do not expose every Moonstone API. In particular, Cephyr does not
enable Chisel GC or ownership enforcement, Syren repair, Plaid's target-program
runtime, Prolific replay or speculation, ExoML native calls, Exotract typed
discovery, or QMR rendering. Those features either need additional language/ABI
modeling or have a different trust and execution contract.

## Driver contracts

- Analysis consumes typed frontend MachineIR before SDK rewrites. It independently
  checks the projected operations and unsupported operations remain unknown.
  It cannot infer C ownership or original source locations absent from that
  IR. Unsupported parts of a module stay unknown. A candidate or heuristic
  finding does not justify a rewrite or establish that a translation unit is
  safe.
- Profiling records bounded events owned by one compile request. It is opt-in
  and separate from compiler stdout and diagnostics. Profiles contain compiler
  stage and input names; they must not be interpreted as program execution
  traces, and timing is not a proof. A report is a separate artifact and has
  the same protection against overwriting compiler inputs as other outputs.
- FFI, templating, and execution tooling must keep their distinct trust
  contracts. Header discovery does not validate ownership, native calls need
  target ABI evidence, Lua/templates are trusted, and Plaid host tasks are not
  a C11 threads implementation.

## Acceptance

Test ordinary and Moonstone-enabled builds, the relocated C/C++/Python install,
invalid options and output paths, and concurrent independent requests. Each
enabled feature must have a functional regression with evidence that its
underlying Moonstone component ran. No default diagnostics, generated code,
or tool order should change merely because Moonstone support was built in.
