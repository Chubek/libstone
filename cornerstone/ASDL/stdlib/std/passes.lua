LIB = asdl.lib.new "std.passes"

local Set = asdl.lib.require('std.collections.set').Set

-- Explicit resource summaries; unknown effects conflict with any nonempty summary.
function LIB.Conflict(a,b)
    if not a then a={unknown=true} end
    if not b then b={unknown=true} end
    local ar,aw,br,bw=a.reads or {},a.writes or {},b.reads or {},b.writes or {}
    if a.unknown then return not not (b.unknown or next(br) or next(bw)) end
    if b.unknown then return not not (next(ar) or next(aw)) end
    for k in pairs(aw) do if br[k] or bw[k] then return true end end
    for k in pairs(bw) do if ar[k] then return true end end
    return false
end

function LIB.New(passes)
    local m={order={},passes={},cache={}}
    for _,p in ipairs(passes) do
        assert(type(p.name)=='string' and p.name~='' and not m.passes[p.name],
            'pass names must be nonempty and unique')
        assert(type(p.run)=='function','missing implementation for '..p.name)
        local deps,seen={},{}
        for i,d in ipairs(p.requires or {}) do
            assert(type(d)=='string' and not seen[d],'invalid dependency of '..p.name)
            deps[i],seen[d]=d,true
        end
        local e=p.effects
        if e~=nil then
            e={unknown=e.unknown,reads=Set.copy(e.reads or {}),writes=Set.copy(e.writes or {})}
        end
        assert(not p.key or type(p.key)=='function','invalid key callback for '..p.name)
        assert(not p.cache or (e and not e.unknown and not next(e.writes)),
            'cached pass must declare no writes or unknown effects: '..p.name)
        m.order[#m.order+1]=p.name
        m.passes[p.name]={run=p.run,requires=deps,cache=p.cache,key=p.key,effects=e}
    end
    LIB.Plan(m)
    return m
end

function LIB.Plan(m, targets)
    local order,seen,path={},{},{}
    local function visit(name)
        if seen[name]=='active' then
            error('pass dependency cycle: '..table.concat(path,' -> ')..' -> '..name,0)
        end
        if seen[name]=='done' then return end
        local p=assert(m.passes[name],'unknown pass: '..tostring(name))
        seen[name]='active'
        path[#path+1]=name
        for _,dep in ipairs(p.requires) do visit(dep) end
        path[#path]=nil
        seen[name]='done'
        order[#order+1]=name
    end
    for _,name in ipairs(targets or m.order) do visit(name) end
    return order
end

function LIB.Clear(m)
    assert(not m.running,'cannot clear a running pass manager')
    m.cache={}
end

local function execute(m,input,o)
    local plan=LIB.Plan(m,o.targets)
    local results,records,staged={},{},{}
    local stats={ran=0,hits=0,order=plan,passes={}}
    for _,name in ipairs(plan) do
        local p=m.passes[name]
        local key
        if p.cache then
            if p.key then key=p.key(input,o.context) else key=o.revision end
            assert(key~=nil,'missing cache key for '..name)
            assert(key==key,'NaN cache key for '..name)
        end
        local entry=p.cache and m.cache[name]
        local deps,values={},{}
        local hit=entry and rawequal(entry.key,key)
        for i,d in ipairs(p.requires) do
            deps[i],values[d]=records[d],results[d]
            if hit and entry.deps[i]~=deps[i] then hit=false end
        end
        if hit then
            stats.hits=stats.hits+1
        else
            entry={key=key,deps=deps,value=p.run(input,values,o.context)}
            if p.cache then staged[name]=entry end
            stats.ran=stats.ran+1
        end
        stats.passes[name]={cached=not not hit}
        records[name],results[name]=entry,entry.value
    end
    for name,entry in pairs(staged) do m.cache[name]=entry end
    return results,stats
end

function LIB.Run(m,input,options)
    assert(not m.running,'reentrant pass manager run')
    m.running=true
    local ok,results,stats=pcall(execute,m,input,options or {})
    m.running=false
    if not ok then error(results,0) end
    return results,stats
end

asdl.signlib(LIB)
return LIB
