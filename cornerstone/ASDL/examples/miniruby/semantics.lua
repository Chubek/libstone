LIB = asdl.lib.new "miniruby.semantics"

local Scope = asdl.lib.require("std.scope").Scope
local Tree = asdl.lib.require("std.tree").Tree
local Control = asdl.lib.require("std.control").Control
local Rewrite = asdl.lib.require("std.rewrite").Rewrite
local D = asdl.lib.require("miniruby.diagnostics")
local Ruby = {}

function Ruby.context()
    return {next_binding=0, next_control=0, classes={Object={methods={}}}}
end

function Ruby.predeclare(body, scope)
    Tree.walk(body, function(node)
        local ty, tag = asdl.type(node), asdl.tag(node)
        if ty == "block" or ty == "method" or tag == "ClassDef" then return false end
        if tag == "LocalTarget" then
            Scope.predeclare(scope, asdl.destruct(node))
        elseif ty == "rescue_arm" then
            local _, name = asdl.destruct(node)
            if name then Scope.predeclare(scope, name) end
        end
    end)
end

function Ruby.parameters(params, scope, ctl, ctx)
    local ranks = {Required=1, Optional=2, Rest=3, KeywordRequired=4,
                   KeywordOptional=4, KeywordRest=5, BlockParam=6}
    local singleton = {Rest=true, KeywordRest=true, BlockParam=true}
    local previous, seen = 0, {}
    for _, param in ipairs(params) do
        local tag = asdl.tag(param)
        local name = asdl.destruct(param)
        local rank = assert(ranks[tag])
        if rank < previous or (singleton[tag] and seen[tag]) then
            D.RubyError("E102", param, name or tag)
        end
        previous = math.max(previous, rank)
        seen[tag] = true
        asdl.logistics(param, scope, ctl, ctx)
    end
end

function Ruby.arguments(args, attached, scope, ctl, ctx)
    local keywords, keyword_phase, passed = {}, false, false
    for _, arg in ipairs(args) do
        local tag = asdl.tag(arg)
        if passed then D.RubyError("E104", arg, tag) end
        if tag == "Keyword" then
            local name = asdl.destruct(arg)
            if keywords[name] then D.RubyError("E103", arg, name) end
            keywords[name] = true
            keyword_phase = true
        elseif tag == "KeywordSplat" then
            keyword_phase = true
        elseif tag == "BlockPass" then
            passed = true
            if attached then D.RubyError("E105", arg) end
        elseif keyword_phase then
            D.RubyError("E106", arg)
        end
        asdl.logistics(arg, scope, ctl, ctx)
    end
    if attached then asdl.logistics(attached, scope, ctl, ctx) end
end

function Ruby.control(node, operation, ctl)
    local target = Control.target(ctl, operation)
    if not target then
        D.RubyError("E201", node, operation)
    else
        asdl.annotate(node, "control-target", target.id)
        asdl.annotate(node, "control-kind", target.kind)
        if operation == "Return" and target.kind == "method" then
            local frame = ctl
            while frame ~= target do
                if frame.kind == "block" then
                    asdl.annotate(node, "nonlocal", true)
                    break
                end
                frame = frame.parent
            end
        end
    end
end

function Ruby.open_class(node, name, superclass, ctx)
    local old = ctx.classes[name]
    if name == "Object" then
        D.RubyError("E304", node, name)
        return {methods={}}
    end
    if superclass == name then D.RubyError("E303", node, name)
    elseif superclass and not ctx.classes[superclass] then
        D.RubyError("E301", node, superclass)
    end
    if old then
        if superclass and superclass ~= old.superclass then
            D.RubyError("E302", node, name)
        end
        return old
    end
    local class = {superclass=superclass or "Object", methods={}}
    ctx.classes[name] = class
    return class
end

function Ruby.register_method(class, class_name, node)
    local kind, name = asdl.destruct(node)
    local key = asdl.tag(kind) .. ":" .. name
    if class.methods[key] then D.RubyWarn("W301", node, name, class_name) end
    class.methods[key] = node
end

function Ruby.normalize(root)
    return Rewrite.bottom_up(root, function(node)
        if asdl.type(node) ~= "expression" then return end
        local tag = asdl.tag(node)
        if tag == "Unless" or tag == "Until" then
            local cond, body, altern = asdl.destruct(node)
            local neg = asdl.make("expression", "Not", {operand=cond}, cond)
            if tag == "Unless" then
                return asdl.make("expression", "If", {cond=neg, body=body, altern=altern}, node)
            end
            return asdl.make("expression", "While", {cond=neg, body=body}, node)
        end
    end)
end

LIB.Ruby = Ruby
asdl.signlib(LIB)
return LIB
