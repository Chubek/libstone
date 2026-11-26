LIB = asdl.lib.new "std.semantics.closures"

local I=asdl.lib.require('std.ir')
function LIB.FreeVariables(e) return I.Free(e) end
function LIB.Captures(e) assert(e.tag=='lambda');local r={} for n in pairs(I.Free(e)) do r[#r+1]=n end;table.sort(r);return r end

asdl.signlib(LIB)
return LIB
