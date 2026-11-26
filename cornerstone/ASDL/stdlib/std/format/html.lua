LIB = asdl.lib.new "std.format.html"

function LIB.Escape(s) return tostring(s):gsub('&','&amp;'):gsub('<','&lt;'):gsub('>','&gt;'):gsub('"','&quot;'):gsub("'",'&#39;') end
function LIB.Pre(s) return '<pre>'..LIB.Escape(s)..'</pre>' end
function LIB.AST(root) local J=asdl.lib.require('std.serialize');return LIB.Pre(J.SExpr(root)) end

asdl.signlib(LIB)
return LIB
