LIB = asdl.lib.new "std.semantics.effects"

local S=asdl.lib.require('std.collections.set').Set
-- Effects are sets of names. Unknown call defaults to unknown (not pure).

function LIB.Effects(root,local_effects,children)
    children=children or asdl.children;
    local function go(n)
        local r=S.copy(local_effects(n) or {unknown=true});
        for _,c in ipairs(children(n)) do
            r=S.union(r,go(c))
        end;
        return r
    end;
    return go(root)
end

function LIB.Pure(effects)
    return next(effects)==nil
end

function LIB.Allowed(effects,allowed)
    return S.subset(effects,allowed)
end

asdl.signlib(LIB)
return LIB
