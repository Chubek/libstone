# ASDL C embedding and DOT

Include `<asdl/ASDL.h>` and link the installed `ASDL::runtime` CMake target.
The header is C-compatible; the runtime and its dependencies are built in C++.
The ABI covers schema loading, inspection, diagnostics and DOT generation. It
does not execute Lua hooks or expose the internal C++ schema representation.

```c
#include <asdl/ASDL.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    asdl_schema *schema = NULL;
    asdl_buffer *dot = NULL;
    asdl_error *error = NULL;
    if (argc != 2) return 2;

    asdl_status status = asdl_load(argv[1], &schema, &error);
    if (status == ASDL_OK)
        status = asdl_schema_dot(schema, ASDL_DOT_SCHEMA, &dot, &error);
    asdl_schema_destroy(schema);

    if (status != ASDL_OK) {
        if (error) fprintf(stderr, "%s\n", asdl_error_message(error));
        asdl_error_destroy(error);
        return 1;
    }
    fwrite(asdl_buffer_data(dot), 1, asdl_buffer_size(dot), stdout);
    asdl_buffer_destroy(dot);
    return 0;
}
```

`asdl_parse` accepts a byte count and requires no trailing NUL. Its optional
source name supplies the filename for diagnostics and the base path for
relative schema imports; use `asdl_load` to load from a file. Lua imports are
described in the model but are not loaded or executed by this API. An
unreadable root file is an `ASDL_IO_ERROR`; parsing, import resolution, and
validation diagnostics return `ASDL_SCHEMA_ERROR`. When a diagnostic is
available, its location uses one-based lines and byte columns.

Result slots are required, initialized to `NULL` by the runtime, and receive
an owned object only on success. Error slots are optional; status remains
authoritative if an error could not be allocated. Do not reuse a populated
slot without first destroying or saving its object. Use the ABI destroy
functions, not `free`. Strings are borrowed from their handles; DOT buffers
have an explicit byte size, are NUL-terminated, and outlive their source
schema. Concurrent reads of a handle are allowed only while it remains
alive. The library retains neither input string storage nor output slots.

`asdl_abi_version()` reports the loaded library's version as a major in the
high 16 bits and a minor in the low 16 bits. Compare the major against
`ASDL_ABI_VERSION`; check the minor if using a function added after ABI 1.0.
Minor additions preserve existing function and constant contracts. A breaking
change requires a new major version.

## DOT views

The runtime and CLI use the same renderer:

- **Schema** (`ASDL_DOT_SCHEMA`): module, product and sum types, constructors,
  fields and common attributes. The labels retain refinements and the ASDL
  cardinality suffixes `?`, `*`, and `+`; a missing suffix means exactly one.
  Dashed edges point from fields to the types they reference.
- **Dependencies** (`ASDL_DOT_DEPENDENCIES`): type-to-type edges labeled by the
  field, constructor, or attribute that makes the reference. Built-in types
  are not nodes; references through generic containers still contribute edges.
- **Imports** (`ASDL_DOT_IMPORTS`): declared schema and Lua imports, including
  bindings and aliases. This view does not trace Lua modules' own dependencies.

Rendering preserves model order and uses synthetic node IDs, so arbitrary
label text neither changes graph syntax nor creates ID collisions. Graphviz
is optional; it is only needed to turn the DOT text into an image.
