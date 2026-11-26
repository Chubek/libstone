LIB = asdl.lib.new "std.types.unification"

-- First-order terms: Var(name), Term(symbol, args). Substitution maps variable names to terms.

function LIB.Var(n)
    return {var=n}
end

function LIB.Term(s,a)
    return {symbol=s,args=a or {}}
end

local function walk(t,s)
    local seen
    while t.var and s[t.var] do
        seen=seen or {}
        assert(not seen[t.var],'cyclic substitution')
        seen[t.var]=true
        t=s[t.var]
    end
    return t
end

function LIB.Apply(t,s)
    local memo,active={},{}
    local function apply(x)
        x=walk(x,s)
        if x.var then return x end
        if memo[x] then return memo[x] end
        assert(not active[x],'cyclic substitution or term')
        active[x]=true
        local a={}
        for i,y in ipairs(x.args) do a[i]=apply(y) end
        local result=LIB.Term(x.symbol,a)
        active[x],memo[x]=nil,result
        return result
    end
    return apply(t)
end

function LIB.Unify(a,b,initial)
    local s={} for k,v in pairs(initial or {}) do
        s[k]=v
    end

    local function occurs(n,t)
        local seen,pending={},{t}
        while #pending>0 do
            local x=walk(table.remove(pending),s)
            if x.var==n then return true end
            if not x.var and not seen[x] then
                seen[x]=true
                for _,y in ipairs(x.args) do pending[#pending+1]=y end
            end
        end;
        return false
    end

    local function u(x,y)
        x,y=walk(x,s),walk(y,s);
        if x==y then return true end
        if x.var then
            if x.var==y.var then
                return true
            end;
            if occurs(x.var,y) then
                return false
            end;
            s[x.var]=y;
            return true
        elseif y.var then
            return u(y,x)
        end;
        if x.symbol~=y.symbol or #x.args~=#y.args then
            return false
        end;
        for i,v in ipairs(x.args) do
            if not u(v,y.args[i]) then
                return false
            end
        end;
        return true
    end

    if u(a,b) then
        return s
    end;
    return nil,'terms do not unify'
end

asdl.signlib(LIB)
return LIB
