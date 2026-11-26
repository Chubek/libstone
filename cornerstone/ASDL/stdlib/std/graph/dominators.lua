LIB = asdl.lib.new "std.graph.dominators"

local S=asdl.lib.require('std.collections.set').Set
local R=asdl.lib.require('std.graph.reachability')
function LIB.Dominators(g,entry)

    local reach,order=R.Reachable(g,{entry});
    local d={};
    for _,v in ipairs(order) do
        d[v]=v==entry and {[v]=true} or S.copy(reach)
    end

    local change=true;
    while change do
        change=false;
        for _,v in ipairs(order) do
            if v~=entry then
                local r;
                for _,p in ipairs(g.incoming[v]) do
                    if reach[p] then
                        r=r and S.intersection(r,d[p]) or S.copy(d[p])
                    end
                end;
                r=r or {};
                r[v]=true;
                if not S.equal(r,d[v]) then
                    d[v]=r;
                    change=true
                end
            end
        end
    end;
    return d
end

function LIB.ImmediateDominators(g,entry,d)
    d=d or LIB.Dominators(g,entry);
    local idom={};
    for v,ds in pairs(d) do
        if v~=entry then
            local best;
            for p in pairs(ds) do
                if p~=v and (best==nil or d[p][best]) then
                    best=p
                end
            end;
            idom[v]=best
        end
    end;
    return idom
end

function LIB.Frontier(g,entry,d,idom)
    d=d or LIB.Dominators(g,entry);
    idom=idom or LIB.ImmediateDominators(g,entry,d);
    local f={};
    for v in pairs(d) do
        f[v]={}
    end;
    for v in pairs(d) do
        for _,p in ipairs(g.incoming[v]) do
            if d[p] then
                local r=p;
                while r~=nil and r~=idom[v] do
                    f[r][v]=true;
                    r=idom[r]
                end
            end
        end
    end;
    return f
end

asdl.signlib(LIB)
return LIB
