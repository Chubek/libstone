LIB = asdl.lib.new "std.transform.ssa"

local C=asdl.lib.require('std.cfg');
local D=asdl.lib.require('std.graph.dominators');
local R=asdl.lib.require('std.graph.reachability')
-- Entry must have no predecessors; all blocks reachable; entry definitions represent parameters.

function LIB.InsertPhi(cfg)

    assert(not cfg.ssa,'already SSA');
    local c=C.Copy(cfg);
    local g=C.Graph(c);
    assert(#g.incoming[c.entry]==0,'SSA requires predecessor-free entry');
    local _,order=R.Reachable(g,{c.entry});
    assert(#order==#c.order,'SSA requires reachable blocks');
    local f=D.Frontier(g,c.entry);
    local sites={};
    for _,b in ipairs(c.order) do
        local block=c.blocks[b];
        assert(#(block.phis or {})==0,'unexpected input phi');
        block.phis={};
        for _,x in ipairs(C.Instructions(block)) do
            for _,v in ipairs(x.defs or {}) do
                sites[v]=sites[v] or {};
                sites[v][b]=true
            end
        end
    end

    local vars={} for v in pairs(sites) do
        vars[#vars+1]=v
    end;
    table.sort(vars,function(a,b)
        if type(a)==type(b) then return a<b end
        return type(a)<type(b)
    end);
    for _,v in ipairs(vars) do
        local q,seen={},{};
        for _,b in ipairs(c.order) do
            if sites[v][b] then
                q[#q+1]=b;
                seen[b]=true
            end
        end;
        local i=1;
        local placed={};
        while i<=#q do
            local b=q[i];
            i=i+1;
            for _,w in ipairs(c.order) do
                if f[b][w] and not placed[w] then
                    placed[w]=true;
                    c.blocks[w].phis[#c.blocks[w].phis+1]={var=v,args={}};
                    if not seen[w] then
                        seen[w]=true;
                        q[#q+1]=w
                    end
                end
            end
        end
    end;
    return c
end

function LIB.ToSSA(cfg)
    local c=LIB.InsertPhi(cfg);
    local g=C.Graph(c);
    local idom=D.ImmediateDominators(g,c.entry);
    local children={};
    for _,b in ipairs(c.order) do
        children[b]={}
    end;
    for _,b in ipairs(c.order) do
        if idom[b] then
            table.insert(children[idom[b]],b)
        end
    end;
    local names,stack={},{};
    local counter=0;
    for _,b in ipairs(c.order) do
        for _,x in ipairs(C.Instructions(c.blocks[b])) do
            for _,v in ipairs(x.defs or {}) do
                names[v]=true
            end;
            for _,v in ipairs(x.uses or {}) do
                names[v]=true
            end
        end
    end

    local function new(v,pushed)
        local n repeat
            counter=counter+1;
            n='$ssa'..counter
        until not names[n];
        names[n]=true;
        stack[v]=stack[v] or {};
        table.insert(stack[v],n);
        pushed[#pushed+1]=v;
        return n
    end

    -- An undefined incoming value is explicitly represented, never silently bound to another definition.

    local undef={};
    local function get(v)
        local s=stack[v];
        if s and #s>0 then
            return s[#s]
        end;
        if not undef[v] then
            local n repeat
                counter=counter+1;
                n='$ssa'..counter
            until not names[n];
            names[n]=true;
            undef[v]=n
        end;
        return undef[v]
    end

    local function visit(b)
        local pushed={};
        local block=c.blocks[b];
        for _,p in ipairs(block.phis) do
            p.dest=new(p.var,pushed)
        end;
        for _,x in ipairs(C.Instructions(block)) do
            for i,v in ipairs(x.uses or {}) do
                x.uses[i]=get(v)
            end;
            for i,v in ipairs(x.defs or {}) do
                x.defs[i]=new(v,pushed)
            end
        end;
        for _,w in ipairs(g.out[b]) do
            for _,p in ipairs(c.blocks[w].phis) do
                p.args[b]=get(p.var)
            end
        end;
        for _,w in ipairs(children[b]) do
            visit(w)
        end;
        for i=#pushed,1,-1 do
            table.remove(stack[pushed[i]])
        end
    end;
    visit(c.entry);
    c.undef=undef;
    c.ssa=true;
    return c
end

function LIB.ValidateSSA(c)
    assert(c.ssa,'expected SSA CFG')
    local g=C.Graph(c)
    local dom=D.Dominators(g,c.entry)
    assert(#g.incoming[c.entry]==0,'SSA requires predecessor-free entry')
    local defs={}
    local function define(v,b,i)
        assert(v~=nil and not defs[v],'duplicate/missing SSA definition: '..tostring(v))
        defs[v]={block=b,index=i}
    end
    local function use(v,b,i)
        local d=assert(defs[v],'missing SSA definition: '..tostring(v))
        assert(not d.block or (d.block==b and d.index<i) or
            (d.block~=b and dom[b][d.block]),'SSA definition does not dominate use: '..tostring(v))
    end
    for _,v in pairs(c.undef or {}) do define(v) end
    for _,b in ipairs(c.order) do
        assert(dom[b],'SSA requires reachable blocks')
        local block=c.blocks[b]
        for _,p in ipairs(block.phis or {}) do
            assert(#g.incoming[b]>0,'phi in predecessor-free block')
            define(p.dest,b,0)
        end
        for i,x in ipairs(C.Instructions(block)) do
            for _,v in ipairs(x.defs or {}) do define(v,b,i) end
        end
    end
    for _,b in ipairs(c.order) do
        local block=c.blocks[b]
        local preds={}
        for _,p in ipairs(g.incoming[b]) do preds[p]=true end
        for _,p in ipairs(block.phis or {}) do
            for pred in pairs(p.args) do assert(preds[pred],'extra phi predecessor') end
            for _,pred in ipairs(g.incoming[b]) do
                assert(p.args[pred]~=nil,'missing phi operand')
                use(p.args[pred],pred,#c.blocks[pred].instrs+2)
            end
        end
        for i,x in ipairs(C.Instructions(block)) do
            for _,v in ipairs(x.uses or {}) do use(v,b,i) end
        end
    end
    return true
end

-- Lower phis by splitting each incoming edge and scheduling parallel copies with temporaries.

function LIB.FromSSA(cfg)
    assert(cfg.ssa);
    local c=C.Copy(cfg);
    local g=C.Graph(c);
    local n=0;
    local names={};
    for _,v in pairs(c.undef or {}) do names[v]=true end
    for _,b in ipairs(c.order) do
        local block=c.blocks[b];
        for _,p in ipairs(block.phis or {}) do
            names[p.dest]=true;
            for _,v in pairs(p.args) do
                names[v]=true
            end
        end;
        for _,x in ipairs(C.Instructions(block)) do
            for _,v in ipairs(x.defs or {}) do
                names[v]=true
            end;
            for _,v in ipairs(x.uses or {}) do
                names[v]=true
            end
        end
    end

    local function fresh(prefix,used)
        local v repeat
            n=n+1;
            v=prefix..n
        until not used[v];
        used[v]=true;
        return v
    end;
    local blockids={} for b in pairs(c.blocks) do
        blockids[b]=true
    end;
    local original=C.Copy(c.order);
    for _,b in ipairs(original) do
        local block=c.blocks[b];
        if #(block.phis or {})>0 then
            for _,pred in ipairs(g.incoming[b]) do
                local edge=fresh('$edge',blockids);
                local instrs,temps={},{};
                for i,p in ipairs(block.phis) do
                    local src=assert(p.args[pred]);
                    local t=fresh('$phi',names);
                    temps[i]=t;
                    instrs[#instrs+1]={op='copy',defs={t},uses={src}}
                end;
                for i,p in ipairs(block.phis) do
                    instrs[#instrs+1]={op='copy',defs={p.dest},uses={temps[i]}}
                end;
                c.blocks[edge]={instrs=instrs,term={op='jump',targets={b},uses={},defs={}}};
                c.order[#c.order+1]=edge;
                for i,w in ipairs(c.blocks[pred].term.targets) do
                    if w==b then
                        c.blocks[pred].term.targets[i]=edge
                    end
                end
            end
        end;
        block.phis=nil
    end;
    c.ssa=nil;
    return c
end

asdl.signlib(LIB)
return LIB
