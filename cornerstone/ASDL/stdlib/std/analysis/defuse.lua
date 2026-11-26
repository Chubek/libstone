LIB = asdl.lib.new "std.analysis.defuse"

local R=asdl.lib.require('std.analysis.reachingdefs');
local C=asdl.lib.require('std.cfg');
local S=asdl.lib.require('std.collections.set').Set
function LIB.DefUse(c)
    local ins,_,defs,per=R.ReachingDefinitions(c);
    local uses,bydef={},{};
    for _,v in ipairs(c.order) do
        local a=S.copy(ins[v]);
        for i,x in ipairs(C.Instructions(c.blocks[v])) do
            for j,u in ipairs(x.uses or {}) do
                local rec={block=v,index=i,operand=j,var=u,definitions={}};
                for d in pairs(a) do
                    if defs[d].var==u then
                        rec.definitions[#rec.definitions+1]=d;
                        bydef[d]=bydef[d] or {};
                        table.insert(bydef[d],rec)
                    end
                end;
                table.sort(rec.definitions);
                uses[#uses+1]=rec
            end;
            for _,d in ipairs(per[v][i]) do
                for old in pairs(a) do
                    if defs[old].var==defs[d].var then
                        a[old]=nil
                    end
                end;
                a[d]=true
            end
        end
    end;
    return {uses=uses,definitions=defs,by_definition=bydef}
end

asdl.signlib(LIB)
return LIB
