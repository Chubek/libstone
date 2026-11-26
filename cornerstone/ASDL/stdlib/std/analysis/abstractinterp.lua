LIB = asdl.lib.new "std.analysis.abstractinterp"

local D=asdl.lib.require('std.analysis.dataflow')
function LIB.Interpret(g,lattice,transfer,boundary,limit)
    return D.Forward(g,{bottom=lattice.Bottom,join=lattice.Join,equal=lattice.Equal,transfer=transfer,boundary=boundary,limit=limit})
end

-- Flat constant-propagation lattice (bottom, const, top).

function LIB.ConstantLattice()
    local function bottom()
        return {tag='bottom'}
    end;
    local function top()
        return {tag='top'}
    end;
    local function eq(a,b)
        return a.tag==b.tag and a.value==b.value
    end;
    return {Bottom=bottom,Top=top,Constant=function(v)
        return {tag='const',value=v}
    end,Equal=eq,Join=function(a,b)
        if a.tag=='bottom' then
            return b
        elseif b.tag=='bottom' or eq(a,b) then
            return a
        else
            return top()
        end
    end,Meet=function(a,b)
        if a.tag=='top' then
            return b
        elseif b.tag=='top' or eq(a,b) then
            return a
        else
            return bottom()
        end
    end}
end

asdl.signlib(LIB)
return LIB
