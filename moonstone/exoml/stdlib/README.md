# StaticFFI standard library

`install_static_ffi(vm)` atomically installs an owned in-memory workspace plus
Types services. Callbacks retain its lifetime. Services run synchronously without
hatch privileges. Mutators validate a candidate copy and return the module name;
failed edits preserve the old declaration.

| Service | Arguments | Result / effect |
| --- | --- | --- |
| `StaticFFI:Module` | name | Create module; duplicates fail |
| `StaticFFI:Header` | module, header | Append include path |
| `StaticFFI:Function` | module, C name, return type, parameter-type list | Add function; arg0… names; unknown effects, privileged by default |
| `StaticFFI:Rename` | module, C name, export name | Set export name |
| `StaticFFI:Policy` | module, C name, privileged, thread-affine, C-strings | Set explicit wrapper policy |
| `StaticFFI:Document` | module, C name, text | Set documentation |
| `StaticFFI:Borrow` | module, C name, argument index | Annotate borrowed input |
| `StaticFFI:Effects` | module, C name, effect names, unknown | Replace effect summary |
| `StaticFFI:Constant` | module, export name, value | Add constant service |
| `StaticFFI:Layout` | module, C record type | Add record probe |
| `StaticFFI:Field` | module, C record type, field name | Add field probe |
| `StaticFFI:ExpectLayout` | module, C record type, size, alignment | Assert expected layout during compilation |
| `StaticFFI:ExpectOffset` | module, C record type, field name, offset | Assert expected field offset |
| `StaticFFI:Modules` | none | Sorted module-name list |
| `StaticFFI:Functions` | module | Export-name list in declaration order |
| `StaticFFI:Headers` | module | Header list |
| `StaticFFI:Validate` | module | Validation-error list |
| `StaticFFI:Describe` | module | Reflection JSON |
| `StaticFFI:Generate` | module | C++20 binding header |
| `StaticFFI:Check` | module | Signature/layout-check source |
| `StaticFFI:Probe` | module | Executable ABI-probe source |
| `Types:Name` | value | null / integer / unsigned / real / boolean / string / strings |
| `Types:String` | value | Display string |
| `Types:IsNull` | value | Boolean |
| `Types:Join` | string list, separator | Joined string |

Effects: `read`, `write`, `allocate`, `free`, `borrow_shared`, `borrow_exclusive`,
`escape`, `foreign_call`, `thread_spawn`, `suspend`. Unknown names fail. Generated
native services always include ForeignCall. Contracts are host annotations,
not inferred proofs.

Workspace limit: 128 modules. Per-module defaults: 4096 functions/constants/
layouts/headers, 128 parameters/fields, 1 MiB declaration text. Header/type
spellings reject code-injection punctuation. Headers are included by generated
code, not read by these services.

`StaticFFI::install` instead provides `Modules`, `Describe`, `Generate`, `Check`
and `Probe` backed by immutable copies of host-created modules. Choose either
that registry or the workspace per VM; namespace collisions fail.
