LIB = asdl.lib.new "std.collections.orderedmap"

local M={}
function M.new() return {keys={},values={}} end
function M.set(m,k,v) assert(k~=nil and v~=nil); if m.values[k]==nil then m.keys[#m.keys+1]=k end; m.values[k]=v; return m end
function M.get(m,k) return m.values[k] end
function M.remove(m,k) local v=m.values[k]; if v~=nil then m.values[k]=nil; for i,x in ipairs(m.keys) do if x==k then table.remove(m.keys,i); break end end end return v end
function M.items(m) local i=0; return function() i=i+1; local k=m.keys[i]; if k~=nil then return k,m.values[k] end end end
LIB.OrderedMap=M

asdl.signlib(LIB)
return LIB
