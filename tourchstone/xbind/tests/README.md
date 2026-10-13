# XBind tests

The XBind tests are scaffold/regimen tests, not generated-binding tests yet.

| Test | Purpose |
| --- | --- |
| `regimen.py` | Verifies the project layout, target matrix, typedef files, JSON schema, hello fixture, CLI probes, and dogfooding spec dump. |
| `qamrpp_module.cpp` | Loads `libqamrpp_xbind.so` into QaMRpp and checks that a spec returns the expected manifest table. |

Run them through CTest:

```sh
cmake -S tourchstone/xbind -B /tmp/opencode/xbind-build -DXBIND_BUILD_TESTS=ON
cmake --build /tmp/opencode/xbind-build --parallel
ctest --test-dir /tmp/opencode/xbind-build --output-on-failure
```

`regimen.py` can also run directly when a built `xbind` CLI is available:

```sh
python3 tourchstone/xbind/tests/regimen.py \
  --root tourchstone/xbind \
  --cli /tmp/opencode/xbind-build/xbind
```
