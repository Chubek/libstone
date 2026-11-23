# Configuring extensions and plugins

The driver reads these optional settings from its selected `Cephyr.toml`
(`--config FILE` takes precedence over `CEPHYR_CONFIG`, then `./Cephyr.toml`):

```toml
[extensions]
lua = ["extensions/constants.lua", "extensions/check.lua"]
lua_env = { BUILD_MODE = "checked", EMPTY = "" }
native = ["plugins/libproject.so"]
```

Both lists take explicit file paths, not directories or library names to
search for. Relative paths resolve against the directory of the selected
configuration file, including a bare filename such as `"libproject.so"`.
Omit a list or use `[]` to load none. Cephyr never discovers extensions by
scanning a directory. Native plugin paths cannot contain `$`, which the
dynamic linker can interpret as a variable.

Lua scripts have the same capabilities and limits as
[`--lua-extension`](README.md). The `lua_env` table requires at least one
configured Lua script. Its values are literal strings (empty is valid) and
the names must be ASCII identifiers matching `[A-Za-z_][A-Za-z0-9_]*`.
Scripts never read the host environment through `lcephyr`; they see only
the values explicitly supplied to them. The configured scripts share the
TOML values, and the CLI scripts share only the `--lua-env` values. Neither
group can see the other's values. Cephyr accepts up to eight Lua scripts
across TOML and the command line, and up to 64 entries per environment
(128 bytes per name, 4096 bytes per value).

For each hook stage, the order is **request SDK, configured native plugins,
configured Lua scripts, CLI Lua scripts**. Array order is preserved within a
list and within a plugin's descriptor. An embedding request that supplies
`cephyr_request.lua` uses that instead of file-based Lua options: combining
it with configured Lua scripts or either driver Lua flag is an error.
All registrations form one SDK pipeline, subject to its existing limits
and conflict checks. Duplicate hook names, duplicate directive name/target
pairs, or competing assembly hooks for the same target are errors, not
overrides. When resolving a directive, an exact target match still wins
over an untargeted match.

## Trusted native SDK plugins

Native plugins are host shared objects with the same privileges as the
compiler. **Use `--allow-native-plugins` only with a trusted configuration
and trusted plugins and dependencies.** The flag must be in the driver's
arguments (including explicit embedding argv); neither `Cephyr.toml` nor an
environment variable grants permission. A nonempty `native` list without
this flag fails compilation before opening any configured native plugin.

Build the plugin for the host running Cephyr, not the compilation target.
Include `<cephyr/CephyrPlugin.h>` and export `cephyr_plugin_v1` with C
linkage. For example, a plugin with no registrations:

```c
#include <cephyr/CephyrPlugin.h>

const cephyr_sdk *cephyr_plugin_v1(void)
{
    static const cephyr_sdk sdk = {
        sizeof(cephyr_sdk), CEPHYR_SDK_ABI_VERSION, NULL, 0, NULL, 0
    };
    return &sdk;
}
```

```sh
cc -shared -fPIC -I<prefix>/include -o plugins/libproject.so project.c
cephyr --config Cephyr.toml --allow-native-plugins -c example.c
```

The versioned entry point returns a non-null, borrowed `cephyr_sdk`
descriptor. Set `struct_size` and `abi_version` as shown and populate the
usual [SDK registration arrays](../SDK.md). The descriptor, its arrays and
strings, and callback state must remain valid throughout the compile. Do
not free or modify the registration data during the call. Entry and
callbacks must not throw across the C ABI, and plugins with mutable
shared state must handle concurrent compile calls.

The driver accepts up to eight native plugin paths. It uses `RTLD_NOW |
RTLD_LOCAL` and holds each successful load open through compilation.
`RTLD_LOCAL` limits symbol visibility; it is not a sandbox. The normal SDK
limits (64 hooks and 64 directives total, including request and Lua
registrations) apply to the combined pipeline.
