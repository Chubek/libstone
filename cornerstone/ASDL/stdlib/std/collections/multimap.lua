LIB = asdl.lib.new "std.collections.multimap"

local M={}
function M.new() return {} end
function M.add(m,k,v) assert(v~=nil); local a=m[k] or {}; m[k]=a; a[#a+1]=v; return m end
function M.get(m,k) local r={} for i,v in ipairs(m[k] or {}) do r[i]=v end return r end
function M.remove(m,k,v) local a=m[k] or {} for i,x in ipairs(a) do if x==v then table.remove(a,i); if #a==0 then m[k]=nil end; return true end end return false end
function M.clear(m,k) m[k]=nil end
LIB.MultiMap=M

asdl.signlib(LIB)
return LIB
