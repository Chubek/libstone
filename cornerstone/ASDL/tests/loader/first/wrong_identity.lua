LIB = asdl.lib.new "wrong"
local LIB = {}
local asdl = {signlib = function() end}
asdl.signlib(LIB)
return LIB
