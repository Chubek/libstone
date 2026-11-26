LIB = asdl.lib.new "std.match.decisiontree"

-- Pattern IR: {tag='wild'}, {tag='ctor',name=...,args={...}}, {tag='lit',value=...}, {tag='or',lhs=...,rhs=...}.

local function wild()
    return {tag='wild'}
end

local function tail(a)
    local r={} for i=2,#a do
        r[#r+1]=a[i]
    end;
    return r
end

local function cat(a,b)
    local r={} for _,x in ipairs(a) do
        r[#r+1]=x
    end;
    for _,x in ipairs(b) do
        r[#r+1]=x
    end;
    return r
end

local function same(a,b)
    return a.tag==b.tag and (a.tag=='ctor' and a.name==b.name or a.tag=='lit' and a.value==b.value)
end

local function expand(rows)
    local out={} for _,r in ipairs(rows) do
        local p=r[1];
        if p and p.tag=='or' then
            for _,x in ipairs(expand({cat({p.lhs},tail(r)),cat({p.rhs},tail(r))})) do
                out[#out+1]=x
            end
        else
            out[#out+1]=r
        end
    end;
    return out
end

local function specialize(rows,p,n)
    local r={} for _,row in ipairs(expand(rows)) do
        local h=row[1];
        if h.tag=='wild' then
            local w={} for i=1,n do
                w[i]=wild()
            end;
            r[#r+1]=cat(w,tail(row))
        elseif same(h,p) then
            r[#r+1]=cat(h.args or {},tail(row))
        end
    end;
    return r
end

local function default(rows)
    local r={} for _,row in ipairs(expand(rows)) do
        if row[1].tag=='wild' then
            r[#r+1]=tail(row)
        end
    end;
    return r
end

-- domains are nil/open ({}) or arrays of {pattern=constructor head, domains=child domains}; use {} for open column.

local function useful(rows,q,domains)

    if #rows==0 then
        return q
    end

    if #q==0 then
        return nil
    end

    rows=expand(rows);
    local p=q[1];
    local rest=tail(q);
    local ds=tail(domains)
    if p.tag=='or' then
        return useful(rows,cat({p.lhs},rest),domains) or useful(rows,cat({p.rhs},rest),domains)
    end

    local domain=domains[1] or {};
    if type(domain)=="function" then
        domain=domain()
    end

    if p.tag~='wild' then
        local d={} for _,c in ipairs(domain) do
            if same(c.pattern,p) then
                d=c.domains;
                break
            end
        end;
        local args=p.args or {};
        if #d==0 then
            for i=1,#args do
                d[i]={}
            end
        end;
        local w=useful(specialize(rows,p,#args),cat(args,rest),cat(d,ds));
        if not w then
            return nil
        end;
        local head={tag=p.tag,name=p.name,value=p.value,args={}};
        for i=1,#args do
            head.args[i]=w[i]
        end;
        return cat({head}, {table.unpack(w,#args+1)})
    end

    local complete=#domain>0
    for _,c in ipairs(domain) do
        local found=false;
        for _,r in ipairs(rows) do
            if same(r[1],c.pattern) then
                found=true;
                break
            end
        end;
        if not found then
            complete=false;
            break
        end
    end

    if complete then
        for _,c in ipairs(domain) do
            local a={} for i=1,#c.domains do
                a[i]=wild()
            end;
            local w=useful(rows,cat({{tag=c.pattern.tag,name=c.pattern.name,value=c.pattern.value,args=a}},rest),domains);
            if w then
                return w
            end
        end;
        return nil
    end

    local w=useful(default(rows),rest,ds);
    if not w then
        return nil
    end

    if #domain>0 then
        for _,c in ipairs(domain) do
            local found=false;
            for _,r in ipairs(rows) do
                if same(r[1],c.pattern) then
                    found=true
                end
            end;
            if not found then
                local a={} for i=1,#c.domains do
                    a[i]=wild()
                end;
                return cat({{tag=c.pattern.tag,name=c.pattern.name,value=c.pattern.value,args=a}},w)
            end
        end
    end

    return cat({wild()},w)
end

LIB.Useful=useful
function LIB.Coverage(arms,domain)
    local rows,red={},{};
    for i,a in ipairs(arms) do
        if not useful(rows,{a.pattern},{domain or {}}) then
            red[#red+1]=i
        end;
        if not a.guarded then
            rows[#rows+1]={a.pattern}
        end
    end;
    local w=useful(rows,{wild()},{domain or {}});
    return {exhaustive=not w,witness=w and w[1],redundant=red}
end

-- Compile ordered arms to a decision DAG. Paths select constructor arguments.
-- Guards are evaluated once, only after the complete pattern succeeds.
function LIB.Build(arms)
    local fail = {tag = 'fail'}
    local function pattern(p, path, yes, no)
        if p.tag == 'wild' then return yes end
        if p.tag == 'or' then
            return pattern(p.lhs, path, yes, pattern(p.rhs, path, yes, no))
        end
        if p.tag == 'lit' then
            return {tag='literal', path=path, value=p.value, yes=yes, no=no}
        end
        assert(p.tag == 'ctor', 'unknown pattern tag')
        local next_test = yes
        for i = #p.args, 1, -1 do
            local child = {table.unpack(path)}
            child[#child+1] = i
            next_test = pattern(p.args[i], child, next_test, no)
        end
        return {tag='constructor', path=path, name=p.name, arity=#p.args,
                yes=next_test, no=no}
    end
    local tree = fail
    for i = #arms, 1, -1 do
        local a = arms[i]
        local accept = {tag='accept', index=i, action=a.action}
        if a.guard then accept={tag='guard', guard=a.guard, yes=accept, no=tree} end
        tree = pattern(a.pattern, {}, accept, tree)
    end
    return tree
end

function LIB.Run(tree, subject, view)
    local function at(path)
        local v = subject
        for _,i in ipairs(path) do
            local _,args = view(v)
            if not args then return nil end
            v = args[i]
        end
        return v
    end
    while tree.tag ~= 'fail' do
        if tree.tag == 'accept' then
            if tree.action then return tree.index, tree.action(subject) end
            return tree.index
        elseif tree.tag == 'guard' then
            tree = tree.guard(subject) and tree.yes or tree.no
        elseif tree.tag == 'literal' then
            tree = at(tree.path) == tree.value and tree.yes or tree.no
        else
            local name,args = view(at(tree.path))
            tree = name == tree.name and args and #args == tree.arity and tree.yes or tree.no
        end
    end
end

function LIB.Compile(arms, view)
    local tree = LIB.Build(arms)
    return function(subject) return LIB.Run(tree, subject, view) end, tree
end

asdl.signlib(LIB)
return LIB
