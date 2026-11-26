LIB = asdl.lib.new "std.sourcemap"

-- Explicit generated -> input provenance, independent of either IR or AST handles.
function LIB.New()
    return {}
end

function LIB.Record(map,generated,input)
    assert(generated~=nil and input~=nil,'source map endpoints must be non-nil')
    if rawequal(generated,input) then return end
    local old=map[generated]
    assert(old==nil or rawequal(old,input),'conflicting source origin')
    map[generated]=input
end

function LIB.Origin(map,generated)
    return map[generated]
end

function LIB.Trace(map,generated)
    assert(generated~=nil,'source map endpoint must be non-nil')
    local path,seen={},{}
    while generated~=nil do
        assert(not seen[generated],'source map cycle')
        seen[generated]=true
        path[#path+1]=generated
        generated=map[generated]
    end
    return path[#path],path
end

function LIB.Compose(previous,next_map)
    local r=LIB.New()
    for k,v in pairs(previous) do LIB.Record(r,k,v) end
    for k,v in pairs(next_map) do LIB.Record(r,k,v) end
    for k in pairs(r) do LIB.Trace(r,k) end
    return r
end

asdl.signlib(LIB)
return LIB
