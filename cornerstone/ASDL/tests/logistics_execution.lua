-- Runtime algorithm tests. Run from the repository root with Lua 5.4.
-- logistics_examples.lua separately exercises the native bridge.
local A = dofile("cornerstone/ASDL/stdlib/tests/host.lua")
local execution = dofile("cornerstone/ASDL/scripts/asdl_execution.lua")(A)
A.define("expr", "Atom", {{name = "value", cardinality = "1"}})
A.define("expr", "Wrap", {{name = "child", cardinality = "?"}})
A.define("root", nil, {{name = "items", cardinality = "+"}})

local atom = A.make("expr", "Atom", {value = 1})
local root = A.make("root", nil, {items = {atom, atom}})
local computed = 0
execution.register({type = "expr", tag = "Atom", phase = "analysis",
    inherited = {"ctx", "unused"}, synthesized = {"seen", "ignored", "answer"}, memoize = true},
    function(node, inherited, hole)
        assert(node == atom and inherited.answer == 7 and hole == nil)
        assert(A.logistics_context().phase == "analysis")
        computed = computed + 1 -- instrumentation only
        return true, nil, inherited.answer
    end)
execution.register({type = "expr", phase = "analysis"}, function() error("selected type-wide fallback") end)
execution.register({type = "root", phase = "analysis", traversal = "children",
    inherited = {"ctx", "unused"}, synthesized = {"answer", "tail"}},
    function(node, ctx, hole, children)
        assert(node == root and A.logistics_context().value == "pass-local")
        assert(hole == nil and children[1].n == 3 and children[1][2] == nil)
        assert(children[1][3] == children[2][3] and children[1][3] == ctx.answer)
        return ctx.answer, nil
    end)
local options = {phase = "analysis", context = "pass-local"}
local result = table.pack(execution.run(options, root, {answer = 7}, nil))
assert(result.n == 2 and result[1] == 7)
assert(computed == 1)
assert(execution.run(options, root, {answer = 7}, nil) == 7 and computed == 2)
local _, mismatch = execution.run(options, root, {answer = 7})
assert(mismatch:find("expected 2 inherited values", 1, true))
local _, fallback = execution.run(options, A.make("expr", "Wrap", {}))
assert(fallback:find("type-wide fallback", 1, true))

-- Unregistered root traverses into both children, but ignores analysis results.
local count = 0
execution.register({type = "expr", phase = "default-walk"}, function(node, arg)
    assert(node == atom and arg == false)
    count = count + 1
    return "not a replacement"
end)
assert(execution.run({phase = "default-walk"}, root, false) == root and count == 2)

-- Checked recursion failure cannot be swallowed by a user pcall.
execution.register({type = "expr", phase = "recursive"}, function(node)
    A.annotate(node, "pending", true)
    pcall(A.logistics, node)
    return node
end)
local bad, depth = execution.run({phase = "recursive", depth_limit = 4}, atom)
assert(not bad and depth:find("recursion limit", 1, true))
assert(A.annotation(atom, "pending") == nil)

A.annotate(atom, "existing", 4)
execution.register({type = "root", phase = "transaction"}, function(node, success)
    A.annotate(atom, "existing", nil)
    A.annotate(atom, "new", false)
    assert(A.annotation(atom, "new") == false and A.annotation(atom, "existing") == nil)
    assert(A.annotations(atom).new == false and A.annotations(atom).existing == nil)
    local copy = A.make("root", nil, {items = {atom}}, node)
    assert(A.origin(copy) == node)
    if not success then
        A.diagnostic.emit({severity = "error", message = "refuse"})
    end
    return copy
end)
local transaction = {phase = "transaction", transform = true}
assert(execution.run(transaction, root, false) == nil)
assert(A.annotation(atom, "existing") == 4 and A.annotation(atom, "new") == nil)
local copy = assert(execution.run(transaction, root, true))
assert(copy ~= root and A.origin(copy) == root)
assert(A.annotation(atom, "existing") == nil and A.annotation(atom, "new") == false)
assert(#(A.destruct(root)) == 2)

execution.register({type = "root", phase = "wrong-type"}, function()
    A.annotate(atom, "pending", true)
    return atom
end)
assert(not execution.run({phase = "wrong-type", transform = true}, root))
assert(A.annotation(atom, "pending") == nil)

execution.register_refinement({type = "expr", tag = "Atom", field = "value", name = "positive!"},
    function(value) return value > 0, "must be positive" end)
execution.register_refinement({type = "root", field = "items", name = "increase!"},
    function(value, parent, field)
        assert(A.type(parent) == "root" and field == "items")
        local n = A.destruct(value)
        if n < 2 then return A.make("expr", "Atom", {value = 2}, value) end
        return true
    end)
local refined = assert(A.refine(root))
local values = A.destruct(refined)
assert(A.destruct(values[1]) == 2 and A.origin(values[1]) == atom and A.origin(refined) == root)
local negative = A.make("expr", "Atom", {value = -1})
local _, reason = A.refine(negative)
assert(reason:find("positive! rejected value: must be positive", 1, true))
local empty = A.make("expr", "Wrap", {})
assert(A.refine(empty) == empty)
execution.register({type = "root", phase = "invalid-output"}, function()
    A.annotate(atom, "pending", true)
    return A.make("root", nil, {items = {negative}})
end)
assert(not execution.run({phase = "invalid-output", transform = true, refinements = true}, root))
assert(A.annotation(atom, "pending") == nil)

print("PASS: ASDL logistics controller")
