# acsem

Check executable Action Semantics specifications and generate standalone
interpreters in **C, C++, Pascal, Python, Rust, Java, and Perl**, or export JSON.
The generator uses Perl core modules only.

```sh
cd acsem
perl acsem.pl --check examples/calculator.acsem
perl acsem.pl --run examples/calculator.acsem --term 'Add(Num(20),Num(22))'
# completed 42

perl acsem.pl --emit python examples/calculator.acsem -o calc.py
python3 calc.py --term 'Mul(Num(6),Num(7))'
# completed 42
```

Read `perldoc ./acsem.pl` for tutorials on arithmetic, lexical scope, storage,
failure and escape; build commands for every target; the notation reference;
limits; and references to the Action Semantics literature.

Keep `acsem.pl`, `Acsem/Emit.pm`, and `runtime/` together. Generated source files
are standalone. Examples are in `examples/`.

```sh
perl -c acsem.pl
podchecker acsem.pl
prove acsem.t backends.t
# Require every target's toolchain instead of skipping unavailable ones:
ACSEM_REQUIRE_TARGETS=1 prove acsem.t backends.t
```

Toolchain paths may be set through `CC`, `CXX`, `FPC`, `PYTHON`, `RUSTC`, `JAVAC`,
and `JAVA`. Compiler outputs are confined to temporary test directories.

License: MIT-0.
