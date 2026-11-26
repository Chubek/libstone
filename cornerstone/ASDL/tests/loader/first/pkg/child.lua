LIB = asdl.lib.derive("pkg.base")
LIB.Label = "derived"
assert(asdl.lib.require("only").label == "second-root")
asdl.lib.require("pkg/base.lua", "1.2.3")
asdl.lib.version(LIB, "3")
asdl.signlib(LIB)
return LIB
