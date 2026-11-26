LIB = asdl.lib.new "std.testing.assertions"

function LIB.Equal(a,b,msg) assert(a==b,msg or ('expected '..tostring(b)..', got '..tostring(a)));return a end
function LIB.Structural(a,b) assert(asdl.lib.require('std.match.structural').Equal(a,b),'structural mismatch');return a end
function LIB.Throws(fn,fragment) local ok,e=pcall(fn);assert(not ok,'expected error');if fragment then assert(tostring(e):find(fragment,1,true),'wrong error: '..tostring(e)) end;return e end

asdl.signlib(LIB)
return LIB
