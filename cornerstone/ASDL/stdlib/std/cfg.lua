LIB = asdl.lib.new "std.cfg"

local G=asdl.lib.require('std.graph.directed').Graph
-- CFG: entry block ID, blocks map, order array. Instructions {op,defs={},uses={},...}; terminators same with targets={}. Variable IDs are strings.

function LIB.New(entry,blocks,order)
    local cfg={entry=entry,blocks=blocks,order=order};
    LIB.Validate(cfg);
    return cfg
end

function LIB.Validate(c)
    assert(c.blocks[c.entry],'missing entry');
    local seen={} for _,id in ipairs(c.order) do
        assert(not seen[id] and c.blocks[id],'bad block order');
        seen[id]=true;
        local b=c.blocks[id];
        assert(type(b.instrs)=='table' and b.term and type(b.term.targets)=='table','bad block');
        for _,t in ipairs(b.term.targets) do
            assert(c.blocks[t],'unknown successor')
        end
    end;
    for id in pairs(c.blocks) do
        assert(seen[id],'unlisted block')
    end;
    return true
end

function LIB.Graph(c)
    LIB.Validate(c);
    local g=G.new(c.order);
    for _,v in ipairs(c.order) do
        for _,w in ipairs(c.blocks[v].term.targets) do
            G.edge(g,v,w)
        end
    end;
    return g
end

function LIB.Copy(x)
    if type(x)~='table' then
        return x
    end;
    local r={} for k,v in pairs(x) do
        r[k]=LIB.Copy(v)
    end;
    return r
end

function LIB.Instructions(b)
    local r={} for i,x in ipairs(b.instrs) do
        r[i]=x
    end;
    r[#r+1]=b.term;
    return r
end

asdl.signlib(LIB)
return LIB
