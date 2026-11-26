LIB = asdl.lib.new "std.types.constraints"

local H=asdl.lib.require('std.types.hm')
function LIB.New() return {} end
function LIB.Equal(c,a,b,node) c[#c+1]={a=a,b=b,node=node};return c end
function LIB.Solve(c) for _,v in ipairs(c) do H.Unify(v.a,v.b,v.node) end;return true end

asdl.signlib(LIB)
return LIB
