LIB = asdl.lib.new "std.analysis.reachingdefs"

local S=asdl.lib.require('std.collections.set').Set;
local D=asdl.lib.require('std.analysis.dataflow');
local C=asdl.lib.require('std.cfg')
function LIB.ReachingDefinitions(c)
    for _,v in ipairs(c.order) do
        assert(#(c.blocks[v].phis or {})==0,'lower SSA phis before reaching-definitions analysis')
    end;
    local g=C.Graph(c);
    local definitions,per={},{};
    local n=0;
    for _,v in ipairs(c.order) do
        per[v]={};
        for i,x in ipairs(C.Instructions(c.blocks[v])) do
            per[v][i]={};
            for _,d in ipairs(x.defs or {}) do
                n=n+1;
                definitions[n]={block=v,index=i,var=d};
                per[v][i][#per[v][i]+1]=n
            end
        end
    end

    local function transfer(v,a)
        a=S.copy(a);
        for _,ds in ipairs(per[v]) do
            for _,id in ipairs(ds) do
                local d=definitions[id];
                for old in pairs(a) do
                    if definitions[old].var==d.var then
                        a[old]=nil
                    end
                end;
                a[id]=true
            end
        end;
        return a
    end

    local ins,outs=D.Forward(g,{bottom=function()
        return {}
    end,join=S.union,equal=S.equal,transfer=transfer});
    return ins,outs,definitions,per
end

asdl.signlib(LIB)
return LIB
