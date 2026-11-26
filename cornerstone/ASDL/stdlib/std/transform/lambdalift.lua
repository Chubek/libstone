LIB = asdl.lib.new "std.transform.lambdalift"

local I=asdl.lib.require('std.ir')
local C=asdl.lib.require('std.transform.closureconv')
-- Lambda lifting makes free variables explicit extra formals. A known application becomes
-- a direct lifted_call; escaping function values retain closure records for partial application.
function LIB.LambdaLift(e)
 local p=C.ClosureConvert(e)
 local function go(x)
  if x.tag=='closure' then return x end
  if x.tag=='invoke' then
   local f,a=go(x.closure),go(x.arg)
   if f.tag=='closure' then return {tag='lifted_call',code=f.code,captures=f.captures,arg=a} end
   return {tag='invoke',closure=f,arg=a}
  end
  return I.Map(x,go)
 end
 p.body=go(p.body)
 for _,f in ipairs(p.functions) do
  f.formals={};for i,n in ipairs(f.captures) do f.formals[i]=n end
  f.formals[#f.formals+1]=f.param
  f.body=go(f.body)
 end
 return p
end

asdl.signlib(LIB)
return LIB
