LIB = asdl.lib.new "std.graph.topology"

function LIB.Topological(g)
    local deg,q,out={},{},{};
    for _,v in ipairs(g.nodes) do
        deg[v]=#g.incoming[v];
        if deg[v]==0 then
            q[#q+1]=v
        end
    end;
    local i=1;
    while i<=#q do
        local v=q[i];
        i=i+1;
        out[#out+1]=v;
        for _,w in ipairs(g.out[v]) do
            deg[w]=deg[w]-1;
            if deg[w]==0 then
                q[#q+1]=w
            end
        end
    end;
    if #out~=#g.nodes then
        local blocked={} for _,v in ipairs(g.nodes) do
            if deg[v]>0 then
                blocked[#blocked+1]=v
            end
        end
        return nil,blocked
    end;
    return out
end

LIB.Topology=LIB.Topological

asdl.signlib(LIB)
return LIB
