# Profile-driven speculation and the `lspecir` Lua IR

SpecIR is a **declarative instruction stream** for communicating profile-derived
speculation. It does not compile to machine code, apply optimizations or install
runtime checks. Prolific emits executable Lua data-construction code; `lspecir`
validates it, and your `lspecir.translator` handlers decide what to do with each
instruction. A translator can produce diagnostics, another IR, compiler hints,
guard descriptions, dashboards, or a text report.

## Capture, cache, speculate, translate

Build the usual Moonstone targets, then try the complete example:

```sh
build-moonstone/prolific/prolific-speculation profile-cache > example.spec.lua
build-moonstone/prolific/prolific speculate profile-cache speculation-example > cached.spec.lua
LUA_PATH='moonstone/prolific/lua/?.lua;;' \
  build-moonstone/prolific/prolific-lua moonstone/prolific/examples/translate.lua cached.spec.lua
```

The example records illustrative observations for all eleven categories. Reusing
the cache/run name is an explicit duplicate error, like other Prolific cache writes.
`prolific speculate CACHE RUN` reads the existing LMDB snapshot and writes only
SpecIR Lua on stdout (errors go to stderr, exit 1). It does not modify the cached
report. An existing ordinary trace with no structured observations produces a
`NO-PROFILES` warning, not fabricated type/branch information.

`prolific-lua` is the vendored Lua command-line interpreter built alongside the
tools, so you do not need a system Lua. The pure Lua `lspecir` library also runs
on Lua 5.3+. Installation places the module at
`PREFIX/share/moonstone/prolific/lua/lspecir.lua`; set `LUA_PATH` accordingly.
The generator lives in `Moonstone::Prolific`, independent of the Lua runtime;
Lua/Kaguya remains the implementation of specialized `lprofcc`/`lprofvm` replay.

### Host observations

```cpp
#include "prolific/Speculation.hpp"
using namespace moonstone::prolific;
Recorder recorder;
Observation type{ProfileKind::Type, "call:17", "build-12/x86-64/workload-A",
                 "receiver", "Integer", "", 0};
for (int i = 0; i < 100; ++i)
    if (observe(recorder, Domain::VM, type, {"input.lang", "ssa:17", 8, 4}) != Code::Ok)
        /* handle collection loss */;
std::string lua_ir = speculate(recorder.snapshot());
```

Call `observe` when the host actually observes the operation. Each call is one
sample, not a synthetic weight. The C ABI equivalents are `prolific_observe`
with `prolific_observation`, and `prolific_speculate`, which returns a string
released by `prolific_free_string`. The existing v1 ABI remains compatible;
these are additive entry points. The C speculation entry point and CLI use the
default policy; C++ `speculate(snapshot, policy)` accepts configured limits and
thresholds.

An observation identifies:

- **site**: stable host site ID (call site, IR operation, boundary, pass, etc.).
- **context**: explicit build/target/workload identity. Include ABI version,
  feature set, module generation, sampling configuration or relevant environment
  identity here. Reuse only when those contexts are genuinely comparable.
- **dimension**: measured operand, metric or contract, e.g. `receiver-type`,
  `rhs`, `return-class`, `elapsed`, `selected-rule`, `boundary-contract`.
- **outcome** for categorical data; **value** and **unit** for numeric data.
- **domain** and original source/IR location supplied by the adapter.

Profile groups never combine different domains, categories, sites, contexts,
dimensions, units or locations. The system cannot infer omitted context or verify
the host's coverage. A profile says what the supplied trace observed, not what
every future execution will do.

| Category | Input and meaning | Generated predicate / use |
| --- | --- | --- |
| `type` | Categorical type identity | `type_is`: consider guarded type specialization |
| `branch` | Exactly `true` or `false` | `branch_is`: favor observed edge, retain condition and cold edge |
| `value` | Signed 64-bit integer and explicit unit/representation | `value_is` or `value_in_range`: consider checked specialization |
| `call` | Stable callee identity/version | `callee_is`: consider guarded direct dispatch/inlining |
| `interoperation` | Host-defined ABI/ownership/effect contract identity | `interop_contract_is`: consider specialized boundary adapter |
| `system` | Host-defined system-call/interaction outcome class | `system_outcome_is`: monitor **after** the effect |
| `cost` | Nonnegative integer measurement with explicit unit | `cost_in_range`: monitor observed cost range after measuring |
| `lowering` | Selected target lowering rule/choice | `lowering_context_is`: consider that choice in the profile's context |
| `allocation` | Nonnegative size measurement | `allocation_is` or `allocation_in_range`: consider size specialization |
| `exception` | Observed exception/fault class | `exception_is`: investigate/monitor after exception creation |
| `custom` | Nonempty host-defined identity | `custom_is`: user-defined speculation semantics |

Numeric profiles require empty `outcome` and a nonempty `unit`. Categorical
profiles require nonempty `outcome`, empty `unit`, and zero `value`. Cost and
allocation do not accept negatives. Floating-point, string-value or composite
value distributions can use a canonical host representation with `custom`; v1's
numeric value profiles are specifically signed integers. These fields are UTF-8
and subject to the recorder's limits. Invalid observations count as collection
loss, so ignoring their return codes cannot produce a falsely complete snapshot.

The reserved event `name` is `prolific.profile/1`, `kind` is `instant`, and
`detail` is six concatenated `<byte-length>:<UTF-8-bytes>` fields: category, site,
context, dimension, outcome, unit. The integer is stored in `Event::value` and
the original location remains in `Event::location`. This reuses the existing
v1 snapshot codec and transactional LMDB persistence without a new cache format.
Use the observation API rather than constructing the wire payload manually.
Unknown `prolific.profile/` versions or malformed payloads fail speculation.
`profile_observation(event)` decodes a structured observation and returns
`nullopt` only for an ordinary, non-profile event.

Specialized `lprofcc`/`lprofvm` `on('instant', callback)` handlers receive an
additional `e.profile` table for these events, with `category`, `site`, `context`,
`dimension`, `outcome`, `unit`, and exact decimal-string `value`. Ordinary events
have no `profile` table. Malformed reserved events encountered during callbacks
fail replay rather than becoming an empty profile.

## Evidence and selection policy

Default policy: at least **32 samples**, at least **950/1000 dominance** for
categorical/equality candidates, at most 1,024 profiles, 64 histogram buckets per
profile, 8,192 total instructions, and 16 MiB output. Integer comparisons avoid
floating-point threshold drift. Count ties below the strict-majority threshold
produce `POLYMORPHIC` warnings. Numeric value/allocation profiles that lack a
dominant value propose the observed inclusive range; cost profiles always propose
range monitoring. These ranges are observations, not proven future bounds.

Histograms retain the first distinct outcomes up to the bucket limit. Subsequent
samples matching retained outcomes still count; samples for other outcomes count
as `omitted`. Counts always account for the full sample population. Any omitted
samples suppress candidate emission for that profile. Numeric min/max and sums
include all observed values even if a histogram is truncated. Overflowed sums are
unavailable (`sum=''`, `sum_overflow=true`), never wrapped.

Collection loss/unclosed spans suppress **all** candidates and add `INCOMPLETE`.
Insufficient samples add `INSUFFICIENT`; non-dominant categorical data add
`POLYMORPHIC`; bucket exhaustion adds `TRUNCATED`. Interoperation, system and
lowering candidates also emit `SEMANTICS-REQUIRED`. Global profile/instruction/
output exhaustion throws without returning a partial IR module. The public
policy rejects unsupported limits (maximum 100,000 profiles, 4,096 buckets/profile,
1,000,000 instructions and 128 MiB output). Limits bound retained data/output,
not total process RSS or temporary input/copy allocations.

Output ordering is deterministic for the same snapshot and policy: profiles are
sorted by domain/category/context/site/dimension/unit/location, histogram buckets
are sorted by identity, and IDs are assigned in this order. The `matches/samples`
ratio is empirical support, **not** calibrated future confidence. `confidence`
is always `heuristic`, `evidence` is `observed`, and `enforced` is always `false`.
There is no automatic cross-run merge, statistical independence claim, drift
model, benchmark attribution, or automatic optimization.

## SpecIR instruction set

Generated source has this form (fields abbreviated here):

```lua
local ir = require('lspecir')
return ir.module {
    version = 1, source_schema = 'moonstone.prolific/1',
    created_unix_ns = '...', complete = true, rejected = '0', open_spans = '0',
    minimum_samples = '32', dominance_permille = 950,
    instructions = {
        ir.profile { id='p1', category='type', site='call:17', -- ...
        },
        ir.guard { id='g1', profile='p1', predicate='type_is', -- ...
        },
        ir.advice { id='a2', profile='p1', guard='g1', -- ...
        },
    },
}
```

The full runnable output is generated by `prolific-speculation`, rather than this
abbreviated schema illustration. Constructors return detached plain data tables
whose `op` is the instruction name. Module construction validates all fields and
references; `ir.validate(module)` also validates an existing table. Unsupported
versions, opcodes, fields, predicates, inconsistent counts/ranges, duplicate IDs,
forward references and missing guard dependencies are rejected.

- **`profile`**: `id`, `category`, `domain`, `site`, `context`, `dimension`, `unit`,
  `location={file,ir_id,line,column}`, `samples`, `omitted`, `first`, `last`,
  `numeric`, `buckets={{value,count},...}`. Numeric profiles also have `minimum`,
  `maximum`, `sum`, `sum_overflow`. `first/last` are event sequences, not spans.
- **`guard`**: `id`, `profile`, `predicate`, `expected`, `minimum`, `maximum`,
  `phase`, `on_miss`, `matches`, `samples`, `evidence='observed'`, `enforced=false`.
  Equality predicates use `expected` and empty range fields; range predicates use
  `minimum/maximum` and empty `expected`. All operands refer to the associated
  profile's site, context, dimension and unit. A lowering guard checks that the
  current lowering context still matches the profile and that the observed rule
  (`expected`) is applicable; the translator must independently establish legality.
- **`advice`**: `id`, `profile`, `guard`, `code`, `message`, `confidence='heuristic'`.
  Every advice requires a preceding guard for the same profile.
- **`warning`**: `id`, `profile`, `code`, `message`. Empty profile ID means a
  module-wide warning. No guard or transformation is implied.

64-bit values, times and counts are canonical decimal **strings**, including
histogram values for numeric profiles. `version`, `dominance_permille` and source
line/column are small exact Lua numbers. This avoids losing precision in foreign
translators. Lua literals escape control bytes, quotes, backslashes and embedded
NULs; outcome text never becomes executable Lua source.

Guard phases/fallbacks are explicit: `before_use/generic_path` for type/value/
custom, `after_condition/cold_path` for branch, `before_dispatch/generic_dispatch`
for call, `before_boundary/generic_boundary` for interoperation,
`after_effect/reprofile` for system, `after_measurement/reprofile` for cost,
`before_lowering/generic_lowering` for lowering, `before_allocation/generic_allocator`
for allocation and `after_exception/generic_handler` for exception. A monitor
after an effect cannot authorize eliminating or moving that effect. Exception
samples do not estimate overall exception frequency without a denominator of
ordinary executions. Empty/zero observations never establish absence of effects.

## User translators

```lua
local ir = require('lspecir')
local program = assert(loadfile(arg[1]))()
local by_id = {}
local translator = ir.translator {
    profile = function(p, context) by_id[p.id] = p end,
    guard = function(g, context)
        local p = by_id[g.profile]
        -- Emit your own guard description using p.site/context/unit and g's
        -- predicate/phase/on_miss. No runtime check has been installed here.
        return {site=p.site, predicate=g.predicate, fallback=g.on_miss}
    end,
    advice = function(a, context) print(a.code, a.message, a.guard) end,
    warning = function(w, context) print(w.code, w.message) end,
}
local results = translator:translate(program, {target='my compiler'})
```

`translate` validates a detached plan and preflights all needed handlers before
the first call. Handlers run in instruction order with a detached instruction and
your unchanged context object. Missing handlers and unknown instructions are
errors, not silent omissions. The return value is a dense sequence of
`{id, op, value}` envelopes; a handler may return nil. Exceptions propagate to
the caller. Translation is synchronous; no task migration or cancellation is
implicit, and side effects already performed by your handlers are your responsibility.

The module library does not automatically load or execute arbitrary source. The
example's `loadfile` runs a trusted generated file in a normal Lua process. As
with other Prolific Lua code, use process isolation for untrusted scripts. Schema
validation is a data contract, not a sandbox or a proof that supplied evidence is
authentic. Guards are a language-neutral instruction to the translator author;
the author determines how to validate, render, implement, defer, or explicitly
decline each speculation.
