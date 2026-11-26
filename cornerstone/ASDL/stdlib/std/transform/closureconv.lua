LIB = asdl.lib.new "std.transform.closureconv"

local I=asdl.lib.require('std.ir');
local C=asdl.lib.require('std.semantics.closures')
-- Output: {body, functions}; closures hold ordered values, lifted code has explicit env params.

function LIB.ClosureConvert(e)
    e=I.Alpha(e);
    local fresh=I.Freshener(e,'$code');
    local funcs={};
    local function go(x)
        if x.tag=='lambda' then
            local name=fresh();
            local captures=C.Captures(x);
            local vals={} for i,n in ipairs(captures) do
                vals[i]=I.Var(n)
            end;
            local body=go(x.body);
            funcs[#funcs+1]={name=name,param=x.param,captures=captures,body=body};
            return {tag='closure',code=name,captures=vals}
        elseif x.tag=='app' then
            return {tag='invoke',closure=go(x.fn),arg=go(x.arg)}
        end;
        return I.Map(x,go)
    end;
    return {body=go(e),functions=funcs}
end

asdl.signlib(LIB)
return LIB
