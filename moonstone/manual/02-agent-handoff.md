# Extending this baseline

Start with `AGENTS.md`, then build and run CTest. Every subsystem exposes a concrete, intentionally narrow API. Avoid assuming any feature is production-grade merely because a header or CMake option exists.

### Chisel example

```cpp
#include <chisel/Chisel.hpp>
using namespace moonstone::chisel;
Function f{{{0,{{Op::Create,1,0},{Op::BorrowShared,1,7},
                {Op::EndBorrow,1,7},{Op::Move,1,0}}, {}}},0};
auto result = analyze(f);
if (!result.converged || !result.diagnostics.empty()) {
    // Treat as a failed/inconclusive safety check.
}
```

### Syren example

```cpp
#include <syren/Syren.hpp>
using namespace moonstone;
syren::Catalog c;
c.add(syren::parse_rule("(error (id E1) (message bad) (pattern (bad ?x)))"));
auto events = c.diagnose(sexpr::Parser("(bad x)").one());
syren::SarifSink sink;
for (const auto& d : events) sink.emit(d);
const auto sarif = sink.serialize();
```

### Supersonic example

See `examples/synthesis.cpp`. Bounded enumeration is not general program synthesis. A returned program matches the supplied examples, not all possible inputs.

### ExoML example

See `examples/exoml.cpp`. The interpreter does not have a built-in ability to spawn subprocesses, load libraries or parse C headers. Bind these explicitly through trusted host services; privileged services require an allowed hatch operation.
