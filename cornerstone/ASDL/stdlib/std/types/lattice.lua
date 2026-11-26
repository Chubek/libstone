LIB = asdl.lib.new "std.types.lattice"

local S=asdl.lib.require('std.collections.set').Set
function LIB.Powerset(universe)
    return {Top=function()
        return S.copy(universe)
    end,Bottom=function()
        return {}
    end,Join=S.union,Meet=S.intersection,Leq=S.subset,Equal=S.equal}
end

function LIB.Finite(elements,leq)
    local function bound(a,b,upper)
        local c={} for _,x in ipairs(elements) do
            if upper and leq(a,x) and leq(b,x) or not upper and leq(x,a) and leq(x,b) then
                c[#c+1]=x
            end
        end;
        for _,x in ipairs(c) do
            local ok=true;
            for _,y in ipairs(c) do
                if not (upper and leq(x,y) or not upper and leq(y,x)) then
                    ok=false;
                    break
                end
            end;
            if ok then
                return x
            end
        end;
        error('not a lattice')
    end;
    assert(#elements>0);
    local top,bottom=elements[1],elements[1];
    for _,x in ipairs(elements) do
        top=bound(top,x,true);
        bottom=bound(bottom,x,false)
    end;
    return {Top=function()
        return top
    end,Bottom=function()
        return bottom
    end,Join=function(a,b)
        return bound(a,b,true)
    end,Meet=function(a,b)
        return bound(a,b,false)
    end,Leq=leq,Equal=function(a,b)
        return leq(a,b) and leq(b,a)
    end}
end

function LIB.Join(l,a,b)
    return l.Join(a,b)
end

function LIB.Meet(l,a,b)
    return l.Meet(a,b)
end

function LIB.Top(l)
    return l.Top()
end

function LIB.Bottom(l)
    return l.Bottom()
end

asdl.signlib(LIB)
return LIB
