LIB = asdl.lib.new "std.analysis.usecounts"

function LIB.UseCounts(cfg)
    local r={} for _,b in ipairs(cfg.order) do
        local block=cfg.blocks[b];
        for _,x in ipairs(block.instrs) do
            for _,v in ipairs(x.uses or {}) do
                r[v]=(r[v] or 0)+1
            end
        end;
        for _,v in ipairs(block.term.uses or {}) do
            r[v]=(r[v] or 0)+1
        end;
        for _,p in ipairs(block.phis or {}) do
            for _,v in pairs(p.args) do
                r[v]=(r[v] or 0)+1
            end
        end
    end;
    return r
end

asdl.signlib(LIB)
return LIB
