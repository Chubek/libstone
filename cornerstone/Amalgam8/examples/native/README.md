# Register a native form in an embedding host

The embedding executable registers `host-sum` before calling
`am8::compile_file`. The source explicitly activates that form with
`(native host-sum)` before using it. Registering a callback in the host does
not enable it for every module. The regular standalone compiler does not
register this example form.

`host-sum` accepts two small integer arguments and lowers them to a
structured C function declaration, `int native_answer(void)`. The host
callback runs at compile time. Registry callbacks are trusted C++ code;
the capability label documents the extension and is not a sandbox.

From the repository root:

```sh
c++ -std=c++20 -I cornerstone/Amalgam8 -I firestone/libdsl \
    cornerstone/Amalgam8/examples/native/host.cpp -o native-host
./native-host cornerstone/Amalgam8/examples/native/Example.am8 > NativeExample.hpp
cc -std=c11 -x c -o read-answer - <<'EOF'
#include "NativeExample.hpp"
int main(void) { return native_answer() != 42; }
EOF
./read-answer
```
