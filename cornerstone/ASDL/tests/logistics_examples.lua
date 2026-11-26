-- Run through asdl-run against each of examples/Mini{,ML,Ruby}.asdl.
-- No mocks: the script creates nodes with the installed schema and Lua bridge.
local function node(ty, tag, fields)
    return assert(asdl.make(ty, tag, fields or {}))
end
local function field(n, name)
    for _, f in ipairs(asdl.fields(n)) do
        if f.name == name then return f.value end
    end
    error("no field named " .. name)
end
local function exp(tag, fields)
    return node("expression", tag, fields)
end
local function int(x)
    return exp("Literal", {value = node("literal", "Integer", {value = x})})
end
local function run(root)
    return asdl.run_logistics({transform = true, refinements = true}, root)
end

local cases = {}

function cases.Mini()
    local condition = exp("Binary", {
        op = node("binop", "Eq"), lhs = int(1), rhs = int(1),
    })
    local branch = node("statement", "Conditional", {
        cond = condition,
        true_arm = node("statement", "Return", {retval = int(1)}),
        false_arm = node("statement", "Return", {retval = int(0)}),
    })
    local fn = node("funcdef", nil, {name = "main", vars = {}, defn = {branch}})
    local root = node("toplevel", nil, {fndef = {fn}, dir = {}})
    local result = assert(run(root))
    local folded = field(field(field(result, "fndef")[1], "defn")[1], "cond")
    assert(asdl.tag(folded) == "Literal")
    assert(asdl.tag(field(folded, "value")) == "Boolean")
    assert(field(field(folded, "value"), "value") == true)
    assert(asdl.origin(folded) == condition)
    assert(asdl.tag(field(branch, "cond")) == "Binary")
    local bad_fn = node("funcdef", nil, {name = "foo", vars = {}, defn = {branch}})
    local bad = node("toplevel", nil, {fndef = {bad_fn}, dir = {}})
    local errors = asdl.diagnostic.count("error")
    assert(not run(bad))
    assert(asdl.diagnostic.count("error") == errors + 1)
end

function cases.MiniML()
    local ref = exp("Reference", {name = "x"})
    local body = exp("Lambda", {param = "x", body = ref})
    local bind = node("binding", "Value", {name = "id", rhs = body})
    local decl = node("declaration", "Define", {bind = bind})
    local root = node("toplevel", nil, {decls = {decl}})
    assert(run(root))
    local inferred = asdl.annotation(body, "inferred-type")
    assert(type(inferred) == "string" and inferred:find("->", 1, true))
    assert(type(asdl.annotation(bind, "type-scheme")) == "string")
    local bad_ref = exp("Reference", {name = "missing"})
    local bad = node("toplevel", nil, {decls = {node("declaration", "Evaluate", {expr = bad_ref})}})
    assert(not run(bad))
    assert(asdl.annotation(bad_ref, "inferred-type") == nil)
    assert(asdl.diagnostic.count("error") > 0)
end

function cases.MiniRuby()
    local function top(expr)
        return node("toplevel", nil, {decls = {node("declaration", "TopExpr", {expr = expr})}})
    end
    local ref = exp("Name", {name = "x"})
    local target = node("target", "LocalTarget", {name = "x"})
    local assign = exp("Assign", {lhs = target, rhs = int(3)})
    local stop = exp("Break")
    local loop = exp("Until", {cond = int(0), body = stop})
    local seq = exp("Sequence", {elems = {assign, ref, loop}})
    local result = assert(run(top(seq)))
    assert(asdl.annotation(ref, "resolution") == "local")
    assert(asdl.annotation(ref, "binding-id") == asdl.annotation(target, "binding-id"))
    local new_seq = field(field(result, "decls")[1], "expr")
    local new_loop = field(new_seq, "elems")[3]
    assert(asdl.tag(new_loop) == "While")
    assert(asdl.tag(field(new_loop, "cond")) == "Not")
    assert(asdl.origin(new_loop) == loop)
    assert(asdl.annotation(stop, "control-target") == asdl.annotation(new_loop, "control-id"))
    local bad_ref = exp("Name", {name = "y"})
    local bad = top(exp("Sequence", {elems = {bad_ref, exp("Return")}}))
    assert(not run(bad))
    assert(asdl.annotation(bad_ref, "resolution") == nil)
    assert(asdl.diagnostic.count("error") > 0)
end

-- Hosts may expose the schema language as an argument. Otherwise identify the
-- schema without parsing it, using constructor sets that differ across cases.
local name = arg and arg[1]
if not cases[name] then
    if pcall(node, "statement", "Nop") then
        name = "Mini"
    elseif pcall(node, "literal", "Unit") then
        name = "MiniML"
    elseif pcall(node, "expression", "Self") then
        name = "MiniRuby"
    end
end
assert(cases[name], "expected the Mini, MiniML or MiniRuby schema")()
