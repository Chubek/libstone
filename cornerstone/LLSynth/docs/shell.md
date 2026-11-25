# `std.shell`

`std.shell` runs shell source through LibShell's POSIX-oriented lexer, parser,
and runtime. Each call starts a fresh `libsh -s` process. There is no `/bin/sh`
fallback.

```text
.language "Shell example"
.import std.shell as Sh

.rule Start
'syntax: EOF
'semantics: let result: %Sh->Result = Sh:run("printf 'one\\ntwo\\n' | tr a-z A-Z"); return result.Stdout;
.end
```

`run(source: String) -> %Result` returns `Status: Int`, `Stdout: String`,
and `Stderr: String`. Output preserves trailing newlines, with separate
stdout and stderr streams. Ordinary nonzero exit statuses, including LibShell
syntax diagnostics (status 2), are results rather than LLSynth exceptions.
If the shell process is killed by a signal, the status is `128 + signal`.
The generated tool's own exit code is independent of `Status`.

## Build and use

The companion is optional; the generator and unrelated generated tools do not
need LibShell. With the vendor snapshot present and a POSIX C++20 toolchain:

```sh
cmake -S cornerstone/LLSynth/stdlib/shell -B build/llsynth-std-shell
cmake --build build/llsynth-std-shell --target llsynth_shell
export LLSYNTH_LIBSH="$PWD/build/llsynth-std-shell/libshell/cli/libsh"
export LD_LIBRARY_PATH="$PWD/build/llsynth-std-shell${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

The default is the Git-tracked snapshot at
`third_party/libglr/third_party/libshell`, with QaMRpp and SerdeTk in its nested
`third_party/` tree. It does not need a prepared build directory, the optional
top-level `third_party/libshell` mirror, or network access. To use a different
complete local snapshot, set `-DLLSYNTH_LIBSHELL_SOURCE_DIR=/path/to/libshell`
when configuring a fresh build.

The aggregate build produces `bin/libsh` and `lib/libllsynth_shell.so`:

```sh
cmake -S cornerstone/LLSynth -B build/llsynth-native
cmake --build build/llsynth-native --target llsynth_shell
```

The focused project above does not configure the other native stdlib modules.

`cmake --install` installs `libsh` and `libllsynth_shell.so` for the standalone
build. On systems where the loader uses `DYLD_LIBRARY_PATH`, set that rather
than `LD_LIBRARY_PATH`. Unless `LLSYNTH_LIBSH` names a different executable, the
bridge finds `libsh` on `PATH`.

Generate a Python 3 tool for lazy runtime loading of the companion:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T python3 \
  -o example.py example.llsynth
python3 example.py
```

For C99 or C11, link the companion explicitly:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T c11 \
  -o example.c example.llsynth
cc -std=c11 example.c -L build/llsynth-std-shell -lllsynth_shell -lm -o example
./example
```

Split C generation uses the same link dependency. C++ generation is not
supported, matching LLSynth's existing native interop restrictions. `--check`
does not compile or load anything. Missing companion libraries are handled
by the normal native interop diagnostics; launch and capture failures raise
`!ShellError`.

## Execution boundaries

* `run` exposes the vendor runtime, not a separate POSIX conformance layer.
  It supports external commands, builtins, pipelines, redirections and
  here-documents, quoting, expansions, functions, and compound commands
  implemented by that runtime. Unsupported or incomplete shell features
  remain vendor limitations; for example it does not implement the `trap`
  builtin, compound pipeline stages, or interactive job control via this API.
  The default snapshot accepts newline command separators and quotes in
  assignment values, and stops the current shell scope on `exit`.
* Scripts start in the host working directory and inherit its environment.
  Shell state does not persist between calls; filesystem changes do. There
  is no positional-argument or stdin parameter: the script is read from
  stdin before execution, so unredirected commands see EOF. Use a shell
  redirection or here-document for command input.
* This is **not a sandbox**. Scripts can launch programs and access files
  and other resources available to the host user. Do not pass untrusted
  script text or interpolate untrusted values into it.
* Source, stdout, and stderr must be NUL-free UTF-8. The source and each
  returned stream are limited to 8 MiB. Streams are collected through
  temporary files, so this is a result-size limit, not a runtime disk quota.
  Use file redirections for binary data or large output. The bridge waits
  synchronously for `libsh` with no timeout, and does not wait for independent
  background jobs or promise to capture their subsequent output.

```sh
LLSYNTH_SHELL_BUILD="$PWD/build/llsynth-std-shell" \
  python3 cornerstone/LLSynth/tests/stdlib/test_shell.py
```

With no `LLSYNTH_SHELL_BUILD`, the suite only checks generation and module
boundaries without requiring a native build.
