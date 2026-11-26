LIB = asdl.lib.new "runtime_error"
error("boom "..asdl.mark())
asdl.signlib(LIB)
return LIB
