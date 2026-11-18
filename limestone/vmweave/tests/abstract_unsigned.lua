return {
  name="AbstractUnsigned", stack={cell="uint64_t", capacity=2},
  fields={{name="last", type="u64"}},
  instructions={
    {name="MAX", abstract="const 18446744073709551615;"},
    {name="WRAP", abstract="const 1; add;"},
    {name="RESTORE", abstract="const 1; sub;"},
    {name="SQUARE", abstract="dup; mul;"},
    {name="MIX", abstract="const 2; mul; const 1; add;"},
    {name="TRAP", abstract="dup; store last; const 0; div;"},
    {name="HALT", abstract="halt;"},
  },
}
