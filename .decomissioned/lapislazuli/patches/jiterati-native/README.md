# Jiterati native-hardening overlay

The enclosing repository ignores `third_party/`. `overlay.tar.gz` preserves
the modified Jiterati source files, regression tests and documentation outside
that ignored directory, with the dependency's license. It contains complete
replacement files for the current local dependency, not a unified diff or a
standalone Jiterati distribution. No pristine tracked baseline was available.

Changes cover ELF32/64 layout and bounded extraction, relocation-aware Linux
host linking, native CLI artifacts, bytecode records/entry points, temporary
file isolation, fixed-width TAC folding and CFG predecessor integrity.
Details and coverage limits are in the archived `docs/native-hardening.md`.

From `lapislazuli/`, regenerate or check against the installed dependency:

```sh
python3 patches/jiterati-native/snapshot.py
python3 patches/jiterati-native/snapshot.py --check
```

The archive fixes ordering, timestamps, permissions and ownership. The JSON
manifest records SHA-256 for every file and the archive. `--source PATH` selects
another Jiterati checkout. Reproduction requires Python's standard library.

Restore into the corresponding dependency checkout from `lapislazuli/`:

```sh
tar -xzf patches/jiterati-native/overlay.tar.gz -C ../third_party/jiterati
python3 patches/jiterati-native/snapshot.py --check
```

Extraction replaces the listed files. For review or merging with another
dependency revision, extract to a separate directory first.

Validation configuration: Linux AMD64, Lua/docs disabled, 46 CTest cases in
normal and UBSan builds. The tests exercise host linking/execution and optional
`readelf`/`objdump`; cross-target ELF layouts and Keystone mode mappings are
structural checks. General TAC-to-native lowering remains incomplete.
