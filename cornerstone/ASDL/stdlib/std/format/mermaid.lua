LIB = asdl.lib.new "std.format.mermaid"

local function esc(x)
    return tostring(x):gsub('&','&amp;'):gsub('"','&quot;'):gsub('<','&lt;'):gsub('>','&gt;'):gsub('\n',' '):gsub('\r',' ')
end

function LIB.DependencyGraph(g,label)
    local out={'flowchart TD'};
    local ids={} for i,v in ipairs(g.nodes) do
        ids[v]='n'..i;
        out[#out+1]=ids[v]..'["'..esc(label and label(v) or v)..'"]'
    end;
    for _,v in ipairs(g.nodes) do
        for _,w in ipairs(g.out[v]) do
            out[#out+1]=ids[v]..' --> '..ids[w]
        end
    end;
    return table.concat(out,'\n')
end

asdl.signlib(LIB)
return LIB
