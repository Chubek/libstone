LIB = asdl.lib.new "std.semantics.environment"

local E={}
function E.new(parent)
    return {parent=parent,values={},types={},ctors={}}
end

local function extend(env,space,name,value)
    local e=E.new(env);
    e[space][name]=value;
    return e
end

local function lookup(env,space,name)
    while env do
        local v=env[space][name];
        if v~=nil then
            return v
        end;
        env=env.parent
    end
end

local function fail(msg,node)
    if node then
        asdl.lib.require('std.diagnostics').Error(msg,node)
    end;
    error(msg,0)
end

function E.bind_value(e,n,s)
    return extend(e,'values',n,s)
end

function E.lookup_value(e,n,node)
    return lookup(e,'values',n) or fail('Unknown value '..n,node)
end

function E.declare_type(e,n,arity,node)
    if lookup(e,'types',n)~=nil then
        fail('Duplicate type '..n,node)
    end;
    return extend(e,'types',n,arity)
end

function E.type_arity(e,n)
    return lookup(e,'types',n)
end

function E.declare_ctor(e,n,s,node)
    if lookup(e,'ctors',n) then
        fail('Duplicate constructor '..n,node)
    end;
    return extend(e,'ctors',n,s)
end

function E.lookup_ctor(e,n,node)
    return lookup(e,'ctors',n) or fail('Unknown constructor '..n,node)
end

function E.constructors(e,tname)
    local r={} while e do
        for n,s in pairs(e.ctors) do
            if s.result.name==tname then
                r[n]=s
            end
        end;
        e=e.parent
    end;
    return r
end

function E.free(e)
    local H=asdl.lib.require('std.types.hm');
    local r,seen={},{};
    while e do
        for n,s in pairs(e.values) do
            if not seen[n] then
                seen[n]=true;
                local f=H.Free(s.type);
                for _,v in ipairs(s.quantified) do
                    f[v]=nil
                end;
                for v in pairs(f) do
                    r[v]=true
                end
            end
        end;
        e=e.parent
    end;
    return r
end

function E.prelude(language)
    local H=asdl.lib.require('std.types.hm');
    local e=E.new();
    for _,n in ipairs({'int','real','bool','string','unit'}) do
        e=E.declare_type(e,n,0)
    end;
    local i,b=H.Named('int'),H.Named('bool');
    for _,n in ipairs({'+','-','*','/','mod'}) do
        e=E.bind_value(e,n,H.Mono(H.Arrow(i,H.Arrow(i,i))))
    end;
    for _,n in ipairs({'<','>','<=','>=','=','<>'}) do
        e=E.bind_value(e,n,H.Mono(H.Arrow(i,H.Arrow(i,b))))
    end;
    return e
end

LIB.Env=E

asdl.signlib(LIB)
return LIB
