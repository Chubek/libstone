LIB = asdl.lib.new "std.types.unification"

-- First-order terms: Var(name), Term(symbol, args). Substitution maps variable names to terms.

function LIB.Var(n)
    return {var=n}
end

function LIB.Term(s,a)
    return {symbol=s,args=a or {}}
end

function LIB.Apply(t,s)
    if t.var then
        return s[t.var] and LIB.Apply(s[t.var],s) or t
    end;
    local a={} for i,x in ipairs(t.args) do
        a[i]=LIB.Apply(x,s)
    end;
    return LIB.Term(t.symbol,a)
end

function LIB.Unify(a,b,initial)
    local s={} for k,v in pairs(initial or {}) do
        s[k]=v
    end

    local function occurs(n,t)
        t=LIB.Apply(t,s);
        if t.var then
            return t.var==n
        end;
        for _,x in ipairs(t.args) do
            if occurs(n,x) then
                return true
            end
        end;
        return false
    end

    local function u(x,y)
        x,y=LIB.Apply(x,s),LIB.Apply(y,s);
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
