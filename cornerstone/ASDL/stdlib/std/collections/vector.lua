LIB = asdl.lib.new "std.collections.vector"

local V={}
function V.new(xs) local v={} for i,x in ipairs(xs or {}) do v[i]=x end return v end
function V.push(v,x) assert(x~=nil); v[#v+1]=x; return v end
function V.pop(v) return table.remove(v) end
function V.insert(v,i,x) assert(x~=nil); table.insert(v,i,x); return v end
function V.remove(v,i) return table.remove(v,i) end
function V.map(v,f) local r={} for i,x in ipairs(v) do local y=f(x,i); assert(y~=nil); r[i]=y end return r end
function V.filter(v,f) local r={} for i,x in ipairs(v) do if f(x,i) then r[#r+1]=x end end return r end
function V.fold(v,a,f) for i,x in ipairs(v) do a=f(a,x,i) end return a end
function V.slice(v,a,b) local r={} for i=a or 1,b or #v do r[#r+1]=v[i] end return r end
function V.concat(a,b) local r=V.new(a) for _,x in ipairs(b) do V.push(r,x) end return r end
function V.index(v,x) for i,y in ipairs(v) do if x==y then return i end end end
LIB.Vector=V

asdl.signlib(LIB)
return LIB
