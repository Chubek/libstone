LIB = asdl.lib.new "std.analysis.dataflow"

local W=asdl.lib.require('std.collections.worklist').Worklist
-- spec: bottom(), join(a,b), equal(a,b), transfer(vertex,value), boundary map, limit.

local function solve(g,s,back)
    local ins,outs={},{};
    for _,v in ipairs(g.nodes) do
        ins[v]=s.bottom();
        outs[v]=s.bottom()
    end;
    local input,output,neighbors,users=ins,outs,g.incoming,g.out
    if back then input,output,neighbors,users=outs,ins,g.out,g.incoming end
    local q=W.new(g.nodes);
    local steps,changes=0,0;
    while not W.empty(q) do
        steps=steps+1;
        assert(steps<=(s.limit or 1000000),'dataflow did not converge');
        local v=W.pop(q);
        local a=s.bottom();
        if s.boundary and s.boundary[v]~=nil then
            a=s.join(a,s.boundary[v])
        end;
        for _,p in ipairs(neighbors[v]) do
            local value=output[p];
            if s.edge then
                if back then
                    value=s.edge(v,p,value)
                else
                    value=s.edge(p,v,value)
                end
            end;
            a=s.join(a,value)
        end;
        local b=s.transfer(v,a);
        local prev=output[v];
        input[v],output[v]=a,b;
        if not s.equal(prev,b) then
            changes=changes+1;
            for _,p in ipairs(users[v]) do
                W.push(q,p)
            end
        end
    end;
    if s.statistics then return ins,outs,{steps=steps,changes=changes} end
    return ins,outs
end

function LIB.Forward(g,s)
    return solve(g,s,false)
end

function LIB.Backward(g,s)
    return solve(g,s,true)
end

function LIB.FixedPoint(initial,step,equal,limit)
    local v=initial;
    for _=1,limit or 10000 do
        local n=step(v);
        if equal(v,n) then
            return n
        end;
        v=n
    end;
    error('fixed point limit exceeded')
end

asdl.signlib(LIB)
return LIB
