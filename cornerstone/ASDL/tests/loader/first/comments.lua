--[==[pretend LIB = asdl.lib.derive "nope"]==]
LIB --[=[don't treat these as tokens]=]
    = asdl.lib.new [[a name]]
LIB.ok = true
LIB.data = [=[asdl.signlib(LIB); return LIB]=]
asdl.signlib(LIB); return LIB; -- trailing comment
