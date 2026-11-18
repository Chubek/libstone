local v = require("vmweave")
return v.vm {
  name = "AbstractCalculator",
  stack = { cell = "int64_t", capacity = 4 },
  fields = { { name = "acc", type = "i64" } },
  instructions = {
    { name="PUSH", operands={"cell"}, abstract="operand 0;" },
    { name="POP", abstract="drop;" },
    { name="ADD", abstract="add;" },
    { name="SUB", abstract="sub;" },
    { name="MUL", abstract="mul;" },
    { name="DIV", abstract="div;" },
    { name="SAVE", abstract="store acc;" },
    { name="LOAD", abstract="load acc;" },
    { name="DUP", abstract="dup;" },
    { name="SWAP", abstract="swap;" },
    { name="EQ", abstract="eq;" },
    { name="LT", abstract="lt;" },
    { name="JZ", operands={"label"}, abstract="jump_zero 0;" },
    { name="JUMP", operands={"label"}, abstract="jump 0;" },
    { name="HALT", abstract="halt;" },
    -- No transient guest stack space: works even when the guest stack is full.
    { name="INC", abstract="const 1; add;" },
    { name="MIN", abstract="const -9223372036854775808;" },
    -- Concrete C can supply behavior outside the abstract instruction subset.
    { name="BIAS", semantics=[[
      if (vm->acc <= INT64_MAX - 2) vm->acc += 2;
      else vm_fail(-20);
    ]] },
  },
}
