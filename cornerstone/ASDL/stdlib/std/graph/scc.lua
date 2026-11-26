LIB = asdl.lib.new "std.graph.scc"

local G=asdl.lib.require('std.graph.directed').Graph
function LIB.Tarjan(g)

    local index,low,stack,on,groups,n={},{},{},{},{},0
    local function visit(v)
        n=n+1;
        index[v]=n;
        low[v]=n;
        stack[#stack+1]=v;
        on[v]=true
        for _,w in ipairs(g.out[v]) do
            if not index[w] then
                visit(w);
                low[v]=math.min(low[v],low[w])
            elseif on[w] then
                low[v]=math.min(low[v],index[w])
            end
        end

        if low[v]==index[v] then
            local c={} repeat
                local w=table.remove(stack);
                on[w]=nil;
                c[#c+1]=w
            until c[#c]==v;
            groups[#groups+1]=c
        end

    end

    for _,v in ipairs(g.nodes) do
        if not index[v] then
            visit(v)
        end
    end
    return groups
end

function LIB.Kosaraju(g)
    local seen,order,groups={},{},{};
    local function dfs(v,edges,out)
        seen[v]=true;
        for _,w in ipairs(edges[v]) do
            if not seen[w] then
                dfs(w,edges,out)
            end
        end;
        out[#out+1]=v
    end;
    for _,v in ipairs(g.nodes) do
        if not seen[v] then
            dfs(v,g.out,order)
        end
    end;
    seen={};
    for i=#order,1,-1 do
        local v=order[i];
        if not seen[v] then
            local c={};
            dfs(v,g.incoming,c);
            groups[#groups+1]=c
        end
    end;
    return groups
end

function LIB.Condensation(g,groups)
    groups=groups or LIB.Tarjan(g);
    local r=G.new();
    local which={};
    for i,c in ipairs(groups) do
        G.add(r,i);
        for _,v in ipairs(c) do
            assert(not which[v]);
            which[v]=i
        end
    end;
    for _,v in ipairs(g.nodes) do
        for _,w in ipairs(g.out[v]) do
            if which[v]~=which[w] then
                G.edge(r,which[v],which[w])
            end
        end
    end;
    return r,which,groups
end

asdl.signlib(LIB)
return LIB
