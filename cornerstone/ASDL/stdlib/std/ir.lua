LIB = asdl.lib.new "std.ir"

-- Pure expression IR: const, var, lambda, app, let, if, prim, tuple.

function LIB.Const(v)
    return {tag='const',value=v}
end

function LIB.Var(n)
    return {tag='var',name=n}
end

function LIB.Lambda(p,b)
    return {tag='lambda',param=p,body=b}
end

function LIB.App(f,a)
    return {tag='app',fn=f,arg=a}
end

function LIB.Let(n,v,b)
    return {tag='let',name=n,value=v,body=b}
end

function LIB.If(c,t,f)
    return {tag='if',cond=c,yes=t,no=f}
end

function LIB.Prim(op,args)
    return {tag='prim',op=op,args=args}
end

function LIB.Tuple(args)
    return {tag='tuple',args=args}
end

function LIB.Map(e,f)
    local t=e.tag;
    if t=='const' or t=='var' then
        return e
    elseif t=='lambda' then
        return LIB.Lambda(e.param,f(e.body))
    elseif t=='app' then
        return LIB.App(f(e.fn),f(e.arg))
    elseif t=='let' then
        return LIB.Let(e.name,f(e.value),f(e.body))
    elseif t=='if' then
        return LIB.If(f(e.cond),f(e.yes),f(e.no))
    elseif t=='prim' or t=='tuple' then
        local a={} for i,x in ipairs(e.args) do
            a[i]=f(x)
        end;
        return t=='prim' and LIB.Prim(e.op,a) or LIB.Tuple(a)
    end;
    error('unknown IR tag '..t)
end

function LIB.Free(e)
    local r={} local function visit(x,b)
        if x.tag=='var' then
            if not b[x.name] then
                r[x.name]=true
            end
        elseif x.tag=='lambda' or x.tag=='let' then
            if x.tag=='let' then
                visit(x.value,b)
            end;
            local n=x.param or x.name;
            local old=b[n];
            b[n]=true;
            visit(x.body,b);
            b[n]=old
        else
            LIB.Map(x,function(y)
                visit(y,b);
                return y
            end)
        end
    end;
    visit(e,{});
    return r
end

function LIB.Names(e)
    local r={} local function visit(x)
        if x.tag=='var' or x.tag=='let' then
            r[x.name]=true
        elseif x.tag=='lambda' then
            r[x.param]=true
        end;
        LIB.Map(x,function(y)
            visit(y);
            return y
        end)
    end;
    visit(e);
    return r
end

function LIB.Freshener(e,prefix,extra)
    local names=LIB.Names(e);
    for n in pairs(extra or {}) do
        names[n]=true
    end;
    local i=0;
    return function()
        local n repeat
            i=i+1;
            n=(prefix or '$t')..i
        until not names[n];
        names[n]=true;
        return n
    end
end

function LIB.Substitute(e,n,value)
    local fv=LIB.Free(value);
    local fresh=LIB.Freshener(e,'$s',LIB.Names(value));
    local function sub(x,k,v,free)
        if x.tag=='var' then
            return x.name==k and v or x
        elseif x.tag=='lambda' or x.tag=='let' then
            local binder=x.param or x.name;
            local body=x.body;
            local val=x.tag=='let' and sub(x.value,k,v,free);
            if binder~=k then
                if free[binder] then
                    local new=fresh();
                    body=sub(body,binder,LIB.Var(new),{[new]=true});
                    binder=new
                end;
                body=sub(body,k,v,free)
            end;
            return x.tag=='lambda' and LIB.Lambda(binder,body) or LIB.Let(binder,val,body)
        end;
        return LIB.Map(x,function(y)
            return sub(y,k,v,free)
        end)
    end;
    return sub(e,n,value,fv)
end

-- Alpha-renaming makes every binder unique while preserving free variables.

function LIB.Alpha(e)
    local fresh=LIB.Freshener(e,'$v');
    local function go(x,env)
        if x.tag=='var' then
            return LIB.Var(env[x.name] or x.name)
        elseif x.tag=='lambda' or x.tag=='let' then
            local n=x.param or x.name;
            local v=x.tag=='let' and go(x.value,env);
            local id=fresh();
            local old=env[n];
            env[n]=id;
            local b=go(x.body,env);
            env[n]=old;
            return x.tag=='lambda' and LIB.Lambda(id,b) or LIB.Let(id,v,b)
        end;
        return LIB.Map(x,function(y)
            return go(y,env)
        end)
    end;
    return go(e,{})
end

function LIB.Atomic(x)
    return x.tag=='const' or x.tag=='var' or x.tag=='lambda'
end

asdl.signlib(LIB)
return LIB
