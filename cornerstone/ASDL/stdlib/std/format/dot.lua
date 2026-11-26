LIB = asdl.lib.new "std.format.dot"

local function quote(x)
    return '"'..tostring(x):gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n'):gsub('\r','\\r')..'"'
end

function LIB.DependencyGraph(g,label)
    local out={'digraph G {'};
    local ids={} for i,v in ipairs(g.nodes) do
        ids[v]='n'..i;
        out[#out+1]=ids[v]..' [label='..quote(label and label(v) or v)..'];'
    end;
    for _,v in ipairs(g.nodes) do
        for _,w in ipairs(g.out[v]) do
            out[#out+1]=ids[v]..' -> '..ids[w]..';'
        end
    end;
    out[#out+1]='}';
    return table.concat(out,'\n')
end

function LIB.ASTGraph(root)
    local G=asdl.lib.require('std.graph.directed').Graph;
    local g=G.new();
    local seen={};
    local function go(n)
        if seen[n] then
            return
        end;
        seen[n]=true;
        G.add(g,n);
        for _,c in ipairs(asdl.children(n)) do
            G.edge(g,n,c);
            go(c)
        end
    end;
    go(root);
    return LIB.DependencyGraph(g,function(n)
        return asdl.tag(n) or asdl.type(n)
    end)
end

function LIB.ControlGraph(c)
    return LIB.DependencyGraph(asdl.lib.require('std.cfg').Graph(c))
end

asdl.signlib(LIB)
return LIB
