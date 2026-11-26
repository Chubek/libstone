LIB = asdl.lib.new "std.collections.set"

local S={}
function S.new(xs) local r={} for _,x in ipairs(xs or {}) do r[x]=true end return r end
function S.copy(a) local r={} for x in pairs(a) do r[x]=true end return r end
function S.add(a,x) a[x]=true; return a end
function S.remove(a,x) a[x]=nil; return a end
function S.has(a,x) return a[x]==true end
function S.union(a,b) local r=S.copy(a) for x in pairs(b) do r[x]=true end return r end
function S.intersection(a,b) local r={} for x in pairs(a) do if b[x] then r[x]=true end end return r end
function S.difference(a,b) local r={} for x in pairs(a) do if not b[x] then r[x]=true end end return r end
function S.subset(a,b) for x in pairs(a) do if not b[x] then return false end end return true end
function S.equal(a,b) return S.subset(a,b) and S.subset(b,a) end
function S.size(a) local n=0 for _ in pairs(a) do n=n+1 end return n end
function S.values(a,less) local r={} for x in pairs(a) do r[#r+1]=x end if less then table.sort(r,less) end return r end
LIB.Set=S

asdl.signlib(LIB)
return LIB
