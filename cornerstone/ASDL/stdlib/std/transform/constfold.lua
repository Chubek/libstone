LIB = asdl.lib.new "std.transform.constfold"

local I=asdl.lib.require('std.ir')
local ops={['+']=function(a,b)
    return a+b
end,['-']=function(a,b)
    return a-b
end,['*']=function(a,b)
    return a*b
end,['/']=function(a,b)
    assert(b~=0);
    return a/b
end,['mod']=function(a,b)
    assert(b~=0);
    return a%b
end,['=']=function(a,b)
    return a==b
end,['<']=function(a,b)
    return a<b
end,['>']=function(a,b)
    return a>b
end,['not']=function(a)
    assert(type(a)=='boolean');
    return not a
end}
function LIB.Primitives()
    local r={} for k,v in pairs(ops) do
        r[k]=v
    end
    return r
end

function LIB.ConstFold(e,primitives)
    primitives=primitives or ops;
    local function go(x)
        x=I.Map(x,go);
        if x.tag=='if' and x.cond.tag=='const' and type(x.cond.value)=='boolean' then
            return x.cond.value and x.yes or x.no
        elseif x.tag=='prim' and primitives[x.op] then
            local a={} for i,v in ipairs(x.args) do
                if v.tag~='const' then
                    return x
                end;
                a[i]=v.value
            end;
            local ok,v=pcall(primitives[x.op],table.unpack(a,1,#x.args));
            if ok and (type(v)=='number' or type(v)=='boolean' or type(v)=='string') then
                return I.Const(v)
            end
        end;
        return x
    end;
    return go(e)
end

asdl.signlib(LIB)
return LIB
