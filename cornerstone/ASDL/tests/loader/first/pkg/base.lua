LIB = asdl.lib.new "pkg.base"

assert(io == nil and os == nil and debug == nil and package == nil)
assert(require == nil and loadfile == nil and load == nil and dofile == nil and rawset == nil)
assert(getmetatable("") == nil and _G.LIB == LIB)
assert(loader_test_module_global == nil)
loader_test_module_global = true
math.abs = "not shared"
asdl.diagnostic.sentinel = "not shared"
asdl.lib.new = "not shared"

asdl.lib.version(LIB, "1.2.3")
LIB.Label, LIB.Run = "first", asdl.mark()

asdl.signlib(LIB)
return LIB
