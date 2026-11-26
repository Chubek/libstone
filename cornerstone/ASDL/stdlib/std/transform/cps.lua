LIB = asdl.lib.new "std.transform.cps"

local I=asdl.lib.require('std.ir')
-- Call-by-value CPS: functions accept argument then continuation, result is lambda k.

function LIB.CPSConvert(e)
    e=I.Alpha(e);
    local fresh=I.Freshener(e,'$k');
    local cv
    local function eval(x,k)
        local n=fresh();
        return cv(x,I.Lambda(n,k(I.Var(n))))
    end

    local function args(xs,i,a,k)
        if i>#xs then
            return k(a)
        end;
        return eval(xs[i],function(v)
            a[i]=v;
            return args(xs,i+1,a,k)
        end)
    end

    cv=function(x,k)
        local t=x.tag;
        if t=='const' or t=='var' then
            return I.App(k,x)
        elseif t=='lambda' then
            local c=fresh();
            return I.App(k,I.Lambda(x.param,I.Lambda(c,cv(x.body,I.Var(c)))))
        elseif t=='let' then
            return cv(x.value,I.Lambda(x.name,cv(x.body,k)))
        elseif t=='app' then
            return eval(x.fn,function(f)
                return eval(x.arg,function(a)
                    return I.App(I.App(f,a),k)
                end)
            end)
        elseif t=='if' then
            return eval(x.cond,function(c)
                return I.If(c,cv(x.yes,k),cv(x.no,k))
            end)
        else
            return args(x.args,1,{},function(a)
                return I.App(k,t=='prim' and I.Prim(x.op,a) or I.Tuple(a))
            end)
        end
    end

    local k=fresh();
    return I.Lambda(k,cv(e,I.Var(k)))
end

asdl.signlib(LIB)
return LIB
