LIB = asdl.lib.new "std.analysis.escape"

-- Conservative graph of value reachability: edges container -> contained, alias edges both ways; roots escape.
local R=asdl.lib.require('std.graph.reachability')
function LIB.EscapeAnalysis(g,roots) return R.Reachable(g,roots) end
function LIB.Escapes(g,roots,v) local s=LIB.EscapeAnalysis(g,roots);return s[v]==true end

asdl.signlib(LIB)
return LIB
