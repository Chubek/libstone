LIB = asdl.lib.new "std.graph.loops"

local D=asdl.lib.require('std.graph.dominators');
local S=asdl.lib.require('std.collections.set').Set
function LIB.NaturalLoops(g,entry)
    local d=D.Dominators(g,entry);
    local loops={};
    for _,u in ipairs(g.nodes) do
        if d[u] then
            for _,v in ipairs(g.out[u]) do
                if d[u][v] then
                    local body={[v]=true,[u]=true};
                    local q=u~=v and {u} or {};
                    while #q>0 do
                        local w=table.remove(q);
                        for _,p in ipairs(g.incoming[w]) do
                            if d[p] and not body[p] then
                                body[p]=true;
                                q[#q+1]=p
                            end
                        end
                    end;
                    loops[#loops+1]={header=v,latch=u,body=body}
                end
            end
        end
    end;
    return loops
end

asdl.signlib(LIB)
return LIB
