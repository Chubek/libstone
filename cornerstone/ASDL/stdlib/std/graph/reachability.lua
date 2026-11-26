LIB = asdl.lib.new "std.graph.reachability"

function LIB.Reachable(g,starts)
    local seen,order={},{};
    local q={} for _,v in ipairs(starts) do
        assert(g.out[v],'unknown start');
        if not seen[v] then
            seen[v]=true;
            q[#q+1]=v
        end
    end;
    local i=1;
    while i<=#q do
        local v=q[i];
        i=i+1;
        order[#order+1]=v;
        for _,w in ipairs(g.out[v]) do
            if not seen[w] then
                seen[w]=true;
                q[#q+1]=w
            end
        end
    end
    return seen,order
end

function LIB.Reachability(g)
    local r={} for _,v in ipairs(g.nodes) do
        r[v]=LIB.Reachable(g,{v})
    end
    return r
end

function LIB.Path(g,a,b)
    assert(g.out[a] and g.out[b]);
    local q={a};
    local seen={[a]=true};
    local pred={};
    local i=1;
    while i<=#q do
        local v=q[i];
        i=i+1;
        if v==b then
            local p={v};
            while v~=a do
                v=pred[v];
                table.insert(p,1,v)
            end
            return p
        end;
        for _,w in ipairs(g.out[v]) do
            if not seen[w] then
                seen[w]=true;
                pred[w]=v;
                q[#q+1]=w
            end
        end
    end
end

asdl.signlib(LIB)
return LIB
