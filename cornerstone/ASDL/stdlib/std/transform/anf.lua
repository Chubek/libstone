LIB = asdl.lib.new "std.transform.anf"

local I=asdl.lib.require('std.ir')
function LIB.ANormalize(e)
    local fresh=I.Freshener(e,'$anf');
    local norm,atom
    atom=function(x,k)
        return norm(x,function(v)
            if I.Atomic(v) then
                return k(v)
            end;
            local n=fresh();
            return I.Let(n,v,k(I.Var(n)))
        end)
    end

    local function args(xs,i,acc,k)
        if i>#xs then
            return k(acc)
        end;
        return atom(xs[i],function(a)
            acc[i]=a;
            return args(xs,i+1,acc,k)
        end)
    end

    norm=function(x,k)
        local t=x.tag;
        if t=='const' or t=='var' then
            return k(x)
        elseif t=='lambda' then
            return k(I.Lambda(x.param,norm(x.body,function(v)
                return v
            end)))
        elseif t=='let' then
            return norm(x.value,function(v)
                return I.Let(x.name,v,norm(x.body,k))
            end)
        elseif t=='app' then
            return atom(x.fn,function(f)
                return atom(x.arg,function(a)
                    return k(I.App(f,a))
                end)
            end)
        elseif t=='if' then
            return atom(x.cond,function(c)
                return k(I.If(c,norm(x.yes,function(v)
                    return v
                end),norm(x.no,function(v)
                    return v
                end)))
            end)
        else
            return args(x.args,1,{},function(a)
                return k(t=='prim' and I.Prim(x.op,a) or I.Tuple(a))
            end)
        end
    end

    return norm(I.Alpha(e),function(v)
        return v
    end)
end

asdl.signlib(LIB)
return LIB
