LIB = asdl.lib.new "std.transform.inline"

local I=asdl.lib.require('std.ir')
-- Beta via let preserves call-by-value argument evaluation, even when unused.

function LIB.Inline(e,limit)
    local remaining=limit or 1000;
    local function go(x)
        x=I.Map(x,go);
        if x.tag=='app' and x.fn.tag=='lambda' and remaining>0 then
            remaining=remaining-1;
            return I.Let(x.fn.param,x.arg,x.fn.body)
        end;
        return x
    end;
    return go(I.Alpha(e))
end

function LIB.BetaReduce(e)
    return LIB.Inline(e)
end

function LIB.EtaReduce(e)
    local function go(x)
        x=I.Map(x,go);
        if x.tag=='lambda' and x.body.tag=='app' and x.body.arg.tag=='var' and x.body.arg.name==x.param and x.body.fn.tag=='lambda' and not I.Free(x.body.fn)[x.param] then
            return x.body.fn
        end;
        return x
    end;
    return go(e)
end

asdl.signlib(LIB)
return LIB
