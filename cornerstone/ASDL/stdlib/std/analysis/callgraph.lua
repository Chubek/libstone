LIB = asdl.lib.new "std.analysis.callgraph"

local G=asdl.lib.require('std.graph.directed').Graph
function LIB.CallGraph(functions,calls)
    local g=G.new();
    for _,f in ipairs(functions) do
        G.add(g,f.name)
    end;
    for _,f in ipairs(functions) do
        for _,callee in ipairs(calls(f)) do
            G.edge(g,f.name,callee)
        end
    end;
    return g
end

asdl.signlib(LIB)
return LIB
