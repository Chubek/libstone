LIB = asdl.lib.new "std.semantics.controlflow"

local C=asdl.lib.require('std.cfg')
local G=asdl.lib.require('std.graph.directed').Graph
local R=asdl.lib.require('std.graph.reachability')
local D=asdl.lib.require('std.graph.dominators')
LIB.Control=asdl.lib.require('std.control').Control
function LIB.Reachable(cfg)
    return R.Reachable(C.Graph(cfg),{cfg.entry})
end

function LIB.Unreachable(cfg)
    local seen=LIB.Reachable(cfg);
    local out={} for _,b in ipairs(cfg.order) do
        if not seen[b] then
            out[#out+1]=b
        end
    end;
    return out
end

function LIB.Dominators(cfg)
    return D.Dominators(C.Graph(cfg),cfg.entry)
end

function LIB.Loops(cfg)
    return asdl.lib.require('std.graph.loops').NaturalLoops(C.Graph(cfg),cfg.entry)
end

-- Compute postdominators relative to a synthetic exit. Non-exiting regions are omitted.

function LIB.PostDominators(cfg)
    local graph=C.Graph(cfg);
    local rev=G.reverse(graph);
    local exit={};
    G.add(rev,exit);
    for _,b in ipairs(cfg.order) do
        if #graph.out[b]==0 then
            G.edge(rev,exit,b)
        end
    end;
    local d=D.Dominators(rev,exit);
    d[exit]=nil;
    for _,s in pairs(d) do
        s[exit]=nil
    end;
    return d
end

asdl.signlib(LIB)
return LIB
