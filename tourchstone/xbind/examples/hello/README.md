# hello XBind fixture

This fixture is intentionally small. It gives the driver one C header and one
`.xbind` spec to resolve before the real parser/generator exists.

Expected scaffold probes:

```sh
xbind --find-spec hello examples/hello
xbind --target Python
```

Future end-to-end checks should generate and run bindings for `hello_add`,
`hello_scale`, and `hello_greet`.
