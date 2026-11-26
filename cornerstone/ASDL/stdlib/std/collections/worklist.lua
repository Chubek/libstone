LIB = asdl.lib.new "std.collections.worklist"

local W={}
function W.new(xs) local w={head=1,tail=0,queue={},pending={}} for _,x in ipairs(xs or {}) do W.push(w,x) end return w end
function W.push(w,x) assert(x~=nil); if w.pending[x] then return false end; w.tail=w.tail+1; w.queue[w.tail]=x; w.pending[x]=true; return true end
function W.pop(w) if w.head>w.tail then return nil end; local x=w.queue[w.head]; w.queue[w.head]=nil; w.head=w.head+1; w.pending[x]=nil; if w.head>w.tail then w.head=1;w.tail=0 end; return x end
function W.empty(w) return w.head>w.tail end
LIB.Worklist=W

asdl.signlib(LIB)
return LIB
