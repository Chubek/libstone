LIB = asdl.lib.new "std.transform.deadcode"

local I=asdl.lib.require('std.ir')
-- Only eliminate evaluation of guaranteed total, effect-free values. Unknown vars may error.

function LIB.DeadEliminate(e)
    local function go(x)
        x=I.Map(x,go);
        if x.tag=='let' and not I.Free(x.body)[x.name] and (x.value.tag=='const' or x.value.tag=='lambda') then
            return x.body
        end;
        if x.tag=='if' and x.cond.tag=='const' and type(x.cond.value)=='boolean' then
            return x.cond.value and x.yes or x.no
        end;
        return x
    end;
    return go(e)
end

asdl.signlib(LIB)
return LIB
