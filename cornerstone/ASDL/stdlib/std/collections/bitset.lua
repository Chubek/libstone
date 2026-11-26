LIB = asdl.lib.new "std.collections.bitset"

local B={}
-- Sparse 32-bit words; nonnegative integer indices.
function B.new(xs) local b={} for _,i in ipairs(xs or {}) do B.add(b,i) end return b end
local function pos(i) assert(math.type(i)=='integer' and i>=0,'invalid bit index'); return i//32,1<<(i%32) end
function B.add(b,i) local w,m=pos(i); b[w]=(b[w] or 0)|m; return b end
function B.remove(b,i) local w,m=pos(i); local v=(b[w] or 0)&(~m); b[w]=v~=0 and v or nil; return b end
function B.has(b,i) local w,m=pos(i); return ((b[w] or 0)&m)~=0 end
function B.union(a,b) local r={} for w,v in pairs(a) do r[w]=v end; for w,v in pairs(b) do r[w]=(r[w] or 0)|v end return r end
function B.intersection(a,b) local r={} for w,v in pairs(a) do local n=v&(b[w] or 0); if n~=0 then r[w]=n end end return r end
function B.difference(a,b) local r={} for w,v in pairs(a) do local n=v&(~(b[w] or 0)); if n~=0 then r[w]=n end end return r end
function B.values(b) local r={} for w,v in pairs(b) do for i=0,31 do if v&(1<<i)~=0 then r[#r+1]=w*32+i end end end table.sort(r); return r end
LIB.BitSet=B

asdl.signlib(LIB)
return LIB
