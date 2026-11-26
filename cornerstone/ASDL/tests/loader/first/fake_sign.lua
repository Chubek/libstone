LIB = asdl.lib.new "fake"
local asdl = {signlib = function() end}
asdl.signlib(LIB)
return LIB
