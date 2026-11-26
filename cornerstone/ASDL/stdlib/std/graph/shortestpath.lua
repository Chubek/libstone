LIB = asdl.lib.new "std.graph.shortestpath"

local Q=asdl.lib.require('std.collections.priorityqueue').PriorityQueue
function LIB.Dijkstra(g,start,weight)
    assert(g.out[start]);
    local dist={[start]=0.0};
    local prev={};
    local q=Q.new(function(a,b)
        return a.d<b.d
    end);
    Q.push(q,{v=start,d=0});
    while Q.peek(q) do
        local x=Q.pop(q);
        if x.d==dist[x.v] then
            for _,w in ipairs(g.out[x.v]) do
                local cost=weight(x.v,w);
                assert(type(cost)=='number' and cost>=0 and cost<math.huge,'invalid edge weight');
                local d=x.d+cost;
                assert(d<math.huge,'distance overflow');
                if not dist[w] or d<dist[w] then
                    dist[w]=d;
                    prev[w]=x.v;
                    Q.push(q,{v=w,d=d})
                end
            end
        end
    end;
    return dist,prev
end

asdl.signlib(LIB)
return LIB
