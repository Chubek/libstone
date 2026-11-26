LIB = asdl.lib.new "std.scope"

local Scope = {}
function Scope.new(parent, kind, owner, ctx)
    return {parent = parent, kind = kind, owner = owner, ctx = ctx,
            bindings = {}, ordered = {}, captures = {}}
end

function Scope.declare(scope, name, kind)
    if scope.bindings[name] then return nil, scope.bindings[name] end
    local ctx = scope.ctx
    ctx.next_binding = (ctx.next_binding or 0) + 1
    local b = {id = ctx.next_binding, name = name, kind = kind, owner = scope}
    scope.bindings[name] = b
    scope.ordered[#scope.ordered + 1] = b
    return b
end

function Scope.lookup(scope, name)
    local s = scope
    while s do
        if s.bindings[name] then return s.bindings[name] end
        s = s.parent
    end
end

function Scope.resolve(scope, name)
    local b = Scope.lookup(scope, name)
    if not b then return nil end
    local s = scope
    while s ~= b.owner do
        -- Only block/lambda scopes have a lexical parent in MiniRuby.
        s.captures[b.id] = b
        s = s.parent
    end
    return b
end

function Scope.predeclare(scope, name)
    return Scope.lookup(scope, name) or Scope.declare(scope, name, "local")
end

local function summary(b)
    return {id = b.id, name = b.name, kind = b.kind}
end

function Scope.finish(scope)
    local locals, captures = {}, {}
    for i, b in ipairs(scope.ordered) do locals[i] = summary(b) end
    for _, b in pairs(scope.captures) do captures[#captures+1] = summary(b) end
    table.sort(captures, function(a,b) return a.id < b.id end)
    asdl.annotate(scope.owner, "locals", locals)
    asdl.annotate(scope.owner, "captures", captures)
end

LIB.Scope = Scope
asdl.signlib(LIB)
return LIB
