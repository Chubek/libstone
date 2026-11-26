LIB = asdl.lib.new "std.testing.property"

-- Local deterministic 32-bit RNG, independent of global math.random.

function LIB.RNG(seed)
    local x=(seed or 1)&0xffffffff;
    if x==0 then
        x=1
    end;
    return function(lo,hi)
        assert(hi>=lo and hi-lo<0xffffffff);
        x=x~((x<<13)&0xffffffff);
        x=x~(x>>17);
        x=x~((x<<5)&0xffffffff);
        return lo+x%(hi-lo+1)
    end
end

function LIB.Generate(gen,seed,size)
    return gen(LIB.RNG(seed),size or 10)
end

function LIB.Integer(lo,hi)
    return function(r)
        return r(lo,hi)
    end
end

function LIB.Array(gen)
    return function(r,size)
        local a={} for i=1,r(0,size) do
            a[i]=gen(r,size)
        end;
        return a
    end
end

function LIB.Shrink(value,shrink,fails,limit)
    local current=value;
    for _=1,limit or 100 do
        local changed=false;
        for _,v in ipairs(shrink(current)) do
            if v~=current and fails(v) then
                current=v;
                changed=true;
                break
            end
        end;
        if not changed then
            break
        end
    end;
    return current
end

function LIB.ForAll(gen,property,opts)
    opts=opts or {};
    local r=LIB.RNG(opts.seed);
    local function fails(v)
        local ok,res=pcall(property,v);
        return not ok or not res,res
    end;
    for i=1,opts.runs or 100 do
        local v=gen(r,math.min(i,opts.size or 100));
        local bad,why=fails(v);
        if bad then
            if opts.shrink then
                v=LIB.Shrink(v,opts.shrink,fails)
            end;
            local _,reason=fails(v);
            return false,{case=v,iteration=i,seed=opts.seed or 1,reason=reason or why}
        end
    end;
    return true
end

asdl.signlib(LIB)
return LIB
