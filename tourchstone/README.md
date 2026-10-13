# Tourchstone

Tourchstone contains the dynamic loading, dynamic invocation, and binding-spec
tools used by the rest of libstone.

| Subproject | Option | Purpose |
| --- | --- | --- |
| `xbind/` | `TOURCHSTONE_BUILD_XBIND` | XBind spec runner/scaffold CLI plus the all-backend binding-spec inventory. |
| `dynload/` | `TOURCHSTONE_BUILD_DYNLOAD` | Native dynamic-loader wrapper with Yellowstone qBFD-backed object inspection. |
| `dyninvoke/` | `TOURCHSTONE_BUILD_DYNINVOKE` | libffi/dyncall-style ABI layout, call, and closure support over qBFD and Sandstone ISA data. |

All options default to `ON` in the Tourchstone aggregate build.

## Build and test

```sh
cmake -S tourchstone -B /tmp/opencode/tourchstone-build \
  -DTOURCHSTONE_BUILD_XBIND=ON \
  -DTOURCHSTONE_BUILD_DYNLOAD=ON \
  -DTOURCHSTONE_BUILD_DYNINVOKE=ON
cmake --build /tmp/opencode/tourchstone-build --parallel
ctest --test-dir /tmp/opencode/tourchstone-build --output-on-failure
```

For XBind-only work:

```sh
cmake -S tourchstone/xbind -B /tmp/opencode/xbind-build -DXBIND_BUILD_TESTS=ON
cmake --build /tmp/opencode/xbind-build --parallel
ctest --test-dir /tmp/opencode/xbind-build --output-on-failure
```

## XBind specs

Tourchstone owns the aggregate `Tourchstone.xbind` plus component specs for
`xbind`, `dynload`, and `dyninvoke`:

| Spec | Surface |
| --- | --- |
| `Tourchstone.xbind` | Aggregate version/status/query functions from `xbind`, `dyninvoke`, and `dynload`. |
| `xbind/Dogfooding.xbind` | Self-binding for the stable `xbind.h` query surface. |
| `dynload/DynLOAD.xbind` | Dynamic-loader and object-inspection query subset. |
| `dyninvoke/DynINVOKE.xbind` | ABI/type/layout query subset. |

Each spec targets D, Go, Java, Lua, NodeJS, OCaml, Python, Ruby, and Rust. The
current specs intentionally avoid callbacks, records, and out-buffer APIs until
the XBind IR and target emitters support those shapes.

See `xbind/MANUAL.md` for the spec authoring guide and current repository-wide
binding coverage.
