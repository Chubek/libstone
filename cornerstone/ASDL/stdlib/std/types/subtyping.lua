LIB = asdl.lib.new "std.types.subtyping"

-- Nominal single/multiple inheritance, arrows (contra/co-variant), tuples (covariant).

function LIB.IsSubtype(a,b,parents)

    if a==b or b=='Top' or a=='Bottom' then
        return true
    end

    if type(a)=='string' and type(b)=='string' then
        local seen={} local function visit(x)
            if x==b then
                return true
            end;
            if seen[x] then
                return false
            end;
            seen[x]=true;
            for _,p in ipairs(parents[x] or {}) do
                if visit(p) then
                    return true
                end
            end;
            return false
        end;
        return visit(a)
    end

    if type(a)~='table' or type(b)~='table' or a.kind~=b.kind then
        return false
    end

    if a.kind=='arrow' then
        return LIB.IsSubtype(b.args[1],a.args[1],parents) and LIB.IsSubtype(a.args[2],b.args[2],parents)
    end

    if a.kind=='tuple' and #a.args==#b.args then
        for i,x in ipairs(a.args) do
            if not LIB.IsSubtype(x,b.args[i],parents) then
                return false
            end
        end;
        return true
    end;
    return false
end

asdl.signlib(LIB)
return LIB
