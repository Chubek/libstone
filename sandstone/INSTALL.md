# Installing Sandstone

## Prerequisites

| Requirement | Minimum | Notes |
| --- | --- | --- |
| CMake | 3.20 | Build configuration |
| C++ compiler | C++20 (GCC 10+, Clang 12+) | Core library, compiler, assembler |
| C compiler | C11 | C ABI + assembler |
| zlib | any | **Required**: default SymAS payload codec |
| Boost.JSON | 1.75+ | Required when building the spec compiler; also used by `Symspec.hpp` |
| Python | 3.9+ | Generating the default SandAS backend; also used by scripts and tests |
| Perl | 5 | Generating the DSymspec and ArchML frontends |
| stock Lua | 5.3+ | Running legacy `compiler/stdspec` sources (any `lua` on `PATH`) |
| DSL runtime | `compiler/stdspec/dsl.lua` | Shipped deterministic collector; C++ validates semantics |
| libzstd | any | Optional: zstd banks (auto-detected) |
| SWIG | 4.0+ | Optional: language bindings |
| Doxygen | any | Optional: API docs (`-DSANDSTONE_BUILD_DOCS=ON`) |
| Bundled Lua/Kaguya | `third_party/lua`, `third_party/kaguya` | Required to build the assembler; linked statically |

Debian/Ubuntu:

```sh
sudo apt install cmake g++ zlib1g-dev libboost-json-dev perl python3 lua5.4 libzstd-dev swig doxygen
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build
```

Useful flags:

```sh
-DSANDSTONE_BUILD_COMPILER=ON   # spec compiler (default ON)
-DSANDSTONE_BUILD_ASSEMBLER=ON  # assembler + sandstone-as (default ON)
-DSANDSTONE_ASSEMBLER_DSYMAS=ON # embed DSymspec profiles in SandAS (default ON)
-DSANDSTONE_BUILD_DSYMSPEC=ON  # also install the standalone generated headers (default OFF)
-DSANDSTONE_BUILD_ARCHML=ON     # .insn profile support (default ON; requires Perl)
-DSANDSTONE_BUILD_TESTS=ON      # smoke + pipeline + ISA tests (default ON)
-DSANDSTONE_BUILD_DOCS=ON       # Doxygen reference (default OFF)
-DSANDSTONE_WITH_MUNE=ON        # legacy DomMune CLI (needs DomWEAVE sources)
-DSANDSTONE_DSL_DIRECTORY=/path/to/compiler/stdspec # optional DSL override
-DCMAKE_INSTALL_PREFIX=/usr/local
```

Without libzstd the build still succeeds; requesting or opening zstd
banks then fails with `DOMSYMAS_ERR_BAD_COMPRESSION`.

SandAS can use built-in profiles or open existing `.symas` banks with
`SANDSTONE_BUILD_COMPILER=OFF`. Building in legacy-only mode with
`SANDSTONE_ASSEMBLER_DSYMAS=OFF` requires an explicit `--bank` or
`--archml`.

JSON documents and generated `stdspec/backend/*.lua` data modules load
without Lua or the DSL runtime. Original `.dsymspec` sources need both.
The CMake DSL directory setting supplies the runtime to source/pipeline
tests; use `--dsl-dir` or the environment variable when running the CLI.

## Install

```sh
cmake --install build
```

Installs libraries (`sandstone`, `sandstone_assembler`,
`sandstone_spec_core`), binaries (`sandstone-spec-compiler`,
`sandstone-as`), public headers, the `stdspec`/`isa` data directories,
and (with docs enabled) the HTML reference. In-tree CMake consumers can use
`add_subdirectory` and link `sandstone_assembler`; its dependencies propagate.
No installed Sandstone `find_package` configuration is currently provided.

An installed C11 SDK consumer can be compiled and run as follows (Linux,
ArchML enabled, default `lib` install directory; add `-lzstd` for zstd builds):

```sh
cmake --install build --prefix /tmp/opencode/sandstone-install
P=/tmp/opencode/sandstone-install
cc -std=c11 -Wall -Wextra -Werror -I"$P/include" -c examples/assembler-sdk.c -o "$P/example.o"
c++ "$P/example.o" -L"$P/lib" -lsandstone_assembler -lsandstone -lsandstone-archml \
    -llibstone_script_lua -lz -lzstd -ldl -lm -pthread -o "$P/example"
"$P/example" "$P/share/sandstone/archml/stdspec/amd64.aml"
```

## Verify

```sh
python3 scripts/spec-compiler.py --list-arches
python3 scripts/spec-compiler.py --validate -o /tmp/x86.symas \
    compiler/stdspec/x86.dsymspec
printf '.text\n_start:\n    nop\n' > /tmp/t.s
./build/assembler/sandstone-as --bank /tmp/x86.symas -o /tmp/t.bin /tmp/t.s
xxd /tmp/t.bin   # 90
```

## Platform notes

- Linux x86-64 is the tested platform. The code is portable C11/C++20
  with C11/C++20 libraries. The CLI uses `getopt`; Windows/MSVC is not
  verified by this change. Lua/Kaguya headers are build-time dependencies,
  not installed consumer requirements.
- Cross assemblers need no target toolchain: the bank fully describes
  the target, and output is validated (`ctest` runs the `.dsymspec` →
  `.symas` → assembled-bytes pipeline for x86 and RISC-V).
