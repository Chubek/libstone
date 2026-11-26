LIB = asdl.lib.new "cycle_a"
asdl.lib.require("cycle_b")
asdl.signlib(LIB)
return LIB
