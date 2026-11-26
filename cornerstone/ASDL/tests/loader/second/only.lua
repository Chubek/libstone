LIB = asdl.lib.new "only"
assert(loader_test_module_global == nil)
assert(asdl.diagnostic.sentinel == "original" and math.abs(-1) == 1)
LIB.label = "second-root"
asdl.signlib(LIB)
return LIB
