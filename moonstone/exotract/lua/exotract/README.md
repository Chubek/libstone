# Lua API (native module registered by LuaRuntime.cpp)

The `exotract` Lua module is registered in `package.loaded` from the native
C++ host before executing the user's contract file. No separate `.so` module
is necessary, and the runtime intentionally disables arbitrary package search.

`exotract.exolib.new(name, config)` accepts `path`, `summary`, `documentation`,
`cstd` (C11), `cache`, `compiler`, `exoml_bind`, `clang_resource_dir`, `includes`,
`defines`, `undefines`, `required_exports`, `preprocessor`, `parser`, and
`header_only` (declaration guard, implementation guard, implementation defines,
dependencies, implementation translation-unit count).

Methods: `Preprocess()` -> self, `Parse()` -> self, `Cache()` -> bool,
`Manifest()` -> JSON text, `Bind(opts)` -> `{Internals={manifest,cdecl,on1x,
implementation,unsafe}}`.

`exotract.global.set(KEY, VALUE)` and `.get(KEY)` are backed by the same Store,
with uppercase identifier keys. In the CLI, `--global KEY=VALUE` supplies the
initial registry. `$GLOBAL{KEY}` strings are expanded in paths, include dirs,
commands and documentation by C++ without interpreting shell syntax.

`exotract.command.exec({"gcc","-E","%file"})` represents an argv vector used
only as an explicit backend fallback; it is not a shell script. Parser fallback
stdout must be complete ExoML CDecl v2 JSON. Unknown methods/backends fail.

Native `exotract.exobind.ON1X` currently produces *unsafe typed import metadata*.
It deliberately does not fabricate executable On1x grammar or claim reviewed
ownership semantics. A custom binder script must return a Lua function that
accepts `Internals` and returns a table.
