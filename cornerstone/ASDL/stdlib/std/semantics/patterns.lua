LIB = asdl.lib.new "std.semantics.patterns"

local D=asdl.lib.require('std.match.decisiontree')
local H=asdl.lib.require('std.types.hm');
local E=asdl.lib.require('std.semantics.environment').Env
local function normalize(p)
    local tag=asdl.tag(p);
    local a,b=asdl.destruct(p);
    if tag=='Wildcard' or tag=='Bind' then
        return {tag='wild'}
    elseif tag=='Alias' then
        return normalize(a)
    elseif tag=='Alternative' then
        return {tag='or',lhs=normalize(a),rhs=normalize(b)}
    elseif tag=='LiteralPattern' then
        local k=asdl.tag(a);
        local v=asdl.destruct(a);
        if k=='Unit' then
            return {tag='ctor',name='()',args={}}
        end;
        return {tag='lit',value=v}
    else
        local args,name;
        if tag=='ProductPattern' then
            args,name=a,'$tuple'
        else
            args,name=b,a
        end;
        local r={} for i,x in ipairs(args) do
            r[i]=normalize(x)
        end;
        return {tag='ctor',name=name,args=r}
    end
end

local function domain(env,t)
    return function()
        t=H.Prune(t);
        local r={};
        if t.kind=='tuple' then
            local d={} for i,x in ipairs(t.args) do
                d[i]=domain(env,x)
            end;
            return {{pattern={tag='ctor',name='$tuple'},domains=d}}
        end;
        if t.kind~='named' then
            return r
        end;
        if t.name=='bool' then
            return {{pattern={tag='lit',value=true},domains={}},{pattern={tag='lit',value=false},domains={}}}
        elseif t.name=='unit' then
            return {{pattern={tag='ctor',name='()'},domains={}}}
        end;
        local cs=E.constructors(env,t.name);
        local names={} for n in pairs(cs) do
            names[#names+1]=n
        end;
        table.sort(names);
        for _,n in ipairs(names) do
            local s=cs[n];
            local map={} for i,v in ipairs(s.quantified) do
                map[v]=t.args[i]
            end;
            local function sub(x)
                x=H.Prune(x);
                if x.kind=='var' then
                    return map[x] or x
                end;
                local a={} for i,y in ipairs(x.args) do
                    a[i]=sub(y)
                end;
                return {kind=x.kind,name=x.name,args=a}
            end;
            local d={} for i,a in ipairs(s.argtypes) do
                d[i]=domain(env,sub(a))
            end;
            r[#r+1]={pattern={tag='ctor',name=n},domains=d}
        end;
        return r
    end
end

local function pretty(p)
    if p.tag=='wild' then
        return '_'
    elseif p.tag=='lit' then
        return tostring(p.value)
    else
        local a={} for i,x in ipairs(p.args or {}) do
            a[i]=pretty(x)
        end;
        return #a==0 and p.name or p.name..'('..table.concat(a,', ')..')'
    end
end

function LIB.CheckCoverage(env,t,arms)
    local a={} for i,arm in ipairs(arms) do
        local p,g=asdl.destruct(arm);
        a[i]={pattern=normalize(p),guarded=g~=nil}
    end;
    local r=D.Coverage(a,domain(env,t));
    for i,k in ipairs(r.redundant) do
        r.redundant[i]=arms[k]
    end;
    r.witness=r.witness and pretty(r.witness);
    return r
end

LIB.Coverage=D.Coverage

asdl.signlib(LIB)
return LIB
