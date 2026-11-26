LIB = asdl.lib.new "std.types.hm"

local nextid=0
function LIB.Fresh()
    nextid=nextid+1;
    return {kind='var',id=nextid}
end

function LIB.Named(name,args)
    return {kind='named',name=name,args=args or {}}
end

function LIB.Arrow(a,b)
    return {kind='arrow',args={a,b}}
end

function LIB.Tuple(xs)
    return {kind='tuple',args=xs}
end

function LIB.Prune(t)
    if t.kind=='var' and t.link then
        t.link=LIB.Prune(t.link);
        return t.link
    end
    return t
end

local function occurs(v,t)
    t=LIB.Prune(t);
    if t==v then
        return true
    end;
    for _,a in ipairs(t.args or {}) do
        if occurs(v,a) then
            return true
        end
    end
    return false
end

function LIB.Pretty(t)

    local names,n={},0;
    local function fmt(x,prec)
        x=LIB.Prune(x);
        if x.kind=='var' then
            if not names[x] then
                n=n+1;
                names[x]="'"..string.char(97+(n-1)%26)..(n>26 and tostring((n-1)//26) or '')
            end;
            return names[x]
        end

        local a={};
        for i,v in ipairs(x.args or {}) do
            a[i]=fmt(v,x.kind=='arrow' and (i==1 and 2 or 1) or 2)
        end

        if x.kind=='arrow' then
            local s=a[1]..' -> '..a[2];
            return prec>1 and '('..s..')' or s
        elseif x.kind=='tuple' then
            return '('..table.concat(a,' * ')..')'
        else
            return #a==0 and x.name or x.name..'<'..table.concat(a,', ')..'>'
        end
    end

    if t.quantified then
        local body=fmt(t.type or t.result,0);
        if t.argtypes then
            local a={} for i,v in ipairs(t.argtypes) do
                a[i]=fmt(v,0)
            end;
            body=table.concat(a,', ')..' => '..body
        end;
        local q={} for i,v in ipairs(t.quantified) do
            q[i]=fmt(v,0)
        end;
        return #q>0 and 'forall '..table.concat(q,' ')..'. '..body or body
    end;
    return fmt(t,0)
end

local function fail(a,b,node,why)
    local msg=(why or 'Cannot unify')..': '..LIB.Pretty(a)..' with '..LIB.Pretty(b);
    if node then
        asdl.lib.require('std.diagnostics').Error(msg,node)
    end;
    error(msg,0)
end

function LIB.Unify(a,b,node)

    a,b=LIB.Prune(a),LIB.Prune(b);
    if a==b then
        return a
    end

    if a.kind=='var' then
        if occurs(a,b) then
            fail(a,b,node,'Occurs check')
        end;
        a.link=b;
        return b
    end

    if b.kind=='var' then
        return LIB.Unify(b,a,node)
    end

    if a.kind~=b.kind or a.name~=b.name or #a.args~=#b.args then
        fail(a,b,node)
    end

    for i,x in ipairs(a.args) do
        LIB.Unify(x,b.args[i],node)
    end;
    return a
end

function LIB.Free(t,out)
    out=out or {};
    t=LIB.Prune(t);
    if t.kind=='var' then
        out[t]=true
    else
        for _,a in ipairs(t.args) do
            LIB.Free(a,out)
        end
    end;
    return out
end

function LIB.Mono(t)
    return {quantified={},type=t}
end

function LIB.CtorScheme(q,args,result)
    return {quantified=q,argtypes=args,result=result}
end

local function subst(t,m)
    t=LIB.Prune(t);
    if t.kind=='var' then
        return m[t] or t
    end;
    local r={kind=t.kind,name=t.name,args={}};
    for i,a in ipairs(t.args) do
        r.args[i]=subst(a,m)
    end;
    return r
end

local function mapping(s)
    local m={} for _,v in ipairs(s.quantified) do
        m[v]=LIB.Fresh()
    end;
    return m
end

function LIB.Instantiate(s)
    return subst(s.type,mapping(s))
end

function LIB.InstantiateCtor(s)
    local m=mapping(s);
    local a={} for i,t in ipairs(s.argtypes) do
        a[i]=subst(t,m)
    end;
    return a,subst(s.result,m)
end

function LIB.Generalize(env,t)
    local free=LIB.Free(t);
    local bound=asdl.lib.require('std.semantics.environment').Env.free(env);
    local q={} for v in pairs(free) do
        if not bound[v] then
            q[#q+1]=v
        end
    end;
    table.sort(q,function(a,b)
        return a.id<b.id
    end);
    return {quantified=q,type=t}
end

asdl.signlib(LIB)
return LIB
