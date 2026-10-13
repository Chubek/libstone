-- Integer local rewrites. One factory instance implements one named objective.
-- Lua integer arithmetic wraps at its native width; mask/sign extension below
-- implements the host's explicitly selected modular 1..64-bit integer domain.
assert(math.maxinteger == 0x7fffffffffffffff, "local rewrites require 64-bit Lua integers")

local rules = {}
local function mask(bits) return bits == 64 and -1 or (1 << bits) - 1 end
local function normalize(n, bits) return n & mask(bits) end
local function literal(n, bits)
    n = normalize(n, bits)
    if bits > 1 and bits < 64 and (n & (1 << (bits-1))) ~= 0 then n = n | ~mask(bits) end
    return {kind="integer", integer=n, bits=bits, type_name=""}
end
local function builtin(t)
    return t and t.kind == "integer" and (t.type_name or "") == ""
        and math.type(t.bits) == "integer" and t.bits >= 1 and t.bits <= 64
end
local function same(a, b)
    if a.kind ~= b.kind then return false end
    if a.kind == "value" then return a.value == b.value end
    return a.kind == "integer" and a.bits == b.bits
        and (a.type_name or "") == (b.type_name or "")
        and normalize(a.integer, a.bits) == normalize(b.integer, b.bits)
end
local function rewrite(i, op, operands)
    i.op, i.operands, i.incoming = op, operands, {}
    return true
end
local function copy(i, operand)
    return rewrite(i, operand.kind == "integer" and "constant" or "copy", {operand})
end
local function constant(i, n) return copy(i, literal(n, i.bits)) end
local function context(ctx)
    local s = {ctx=ctx, defs={}, instructions={}, positions={}, owners={}}
    local blocks = {}
    for id in pairs(ctx.blocks) do blocks[#blocks+1] = id end
    table.sort(blocks)
    for _, id in ipairs(blocks) do
        for n, inst in ipairs(ctx.blocks[id]) do
            s.instructions[#s.instructions+1] = inst
            s.positions[inst], s.owners[inst] = n, id
            if inst.result_id then s.defs[inst.result_id] = inst end
        end
    end
    function s:type(o)
        return o.kind == "value" and self.ctx.value_types[o.value] or o
    end
    function s:eligible(i)
        if not i.result_id or not builtin(i) then return false end
        for _, o in ipairs(i.operands) do
            if not builtin(self:type(o)) then return false end
        end
        return true
    end
    function s:definition(o, op)
        local d = o.kind == "value" and self.defs[o.value]
        if d and d.op == op and self:eligible(d) then return d end
    end
    function s:constant(o)
        local seen = {}
        while o.kind == "value" do
            if seen[o.value] then return end
            seen[o.value] = true
            local d = self.defs[o.value]
            if not d or not self:eligible(d) or (d.op ~= "constant" and d.op ~= "copy") then return end
            o = d.operands[1]
        end
        if builtin(o) then return normalize(o.integer, o.bits), o.bits end
    end
    function s:is(o, n, bits)
        local v, width = self:constant(o)
        return v ~= nil and width == bits and v == normalize(n, bits)
    end
    function s:split(i, op)
        if i.op ~= op then return end
        for n=1,2 do
            local v, bits = self:constant(i.operands[n])
            if v ~= nil and bits == i.bits then return i.operands[3-n], v end
        end
    end
    function s:replace_uses(id, operand)
        local changed = false
        for _, use in ipairs(self.instructions) do
            for n, o in ipairs(use.operands) do
                if o.kind == "value" and o.value == id then
                    use.operands[n] = operand; changed = true
                end
            end
        end
        return changed
    end
    return s
end

local comparison = {eq=true, ne=true, lt=true, le=true, gt=true, ge=true}
local function nonnegative_signed(n, bits)
    return n >= 0 and (bits == 64 or n < (1 << (bits-1)))
end
local function fold(s, i, op)
    if i.op ~= op then return false end
    local a, bits = s:constant(i.operands[1])
    if a == nil then return false end
    local b, bbits
    if #i.operands == 2 then
        b, bbits = s:constant(i.operands[2])
        if b == nil or bits ~= bbits then return false end
    end
    if not comparison[op] and bits ~= i.bits then return false end
    local r
    if op == "add" then r = a + b
    elseif op == "sub" then r = a - b
    elseif op == "mul" then r = a * b
    elseif op == "and" then r = a & b
    elseif op == "or" then r = a | b
    elseif op == "xor" then r = a ~ b
    elseif op == "neg" then r = -a
    elseif op == "not" then r = ~a
    elseif op == "div" or op == "mod" then
        -- Signedness is not encoded by Firestone Type. Fold only the common
        -- nonnegative signed/unsigned domain, never zero divisors or MIN/-1.
        if bits < 2 or not nonnegative_signed(a, bits) or not nonnegative_signed(b, bits) or b == 0 then return false end
        r = op == "div" and a // b or a % b
    elseif op == "shl" or op == "shr" then
        if bits < 2 or b < 0 or b >= bits then return false end
        if op == "shr" and not nonnegative_signed(a, bits) then return false end
        r = op == "shl" and a << b or a >> b
    elseif comparison[op] then
        if op ~= "eq" and op ~= "ne" and
            (not nonnegative_signed(a, bits) or not nonnegative_signed(b, bits)) then return false end
        if op == "eq" then r = a == b
        elseif op == "ne" then r = a ~= b
        elseif op == "lt" then r = a < b
        elseif op == "le" then r = a <= b
        elseif op == "gt" then r = a > b
        else r = a >= b end
        r = r and 1 or 0
    end
    return constant(i, r)
end
for _, op in ipairs({"add", "sub", "mul", "div", "mod", "and", "or", "xor",
                     "shl", "shr", "neg", "not", "eq", "ne", "lt", "le", "gt", "ge"}) do
    rules["fold_" .. op] = function(s, i) return fold(s, i, op) end
end

local function identity(name, op, n, commutative, result)
    rules[name] = function(s, i)
        if i.op ~= op or ((op == "div" or op == "mod") and i.bits < 2) then return false end
        for position=2, commutative and 1 or 2, -1 do
            if s:is(i.operands[position], n, i.bits) then
                return result and constant(i, result) or copy(i, i.operands[3-position])
            end
        end
        return false
    end
end
identity("add_zero", "add", 0, true)
identity("sub_zero", "sub", 0, false)
identity("mul_one", "mul", 1, true)
identity("mul_zero", "mul", 0, true, 0)
identity("div_one", "div", 1, false)
identity("mod_one", "mod", 1, false, 0)
identity("and_zero", "and", 0, true, 0)
identity("and_all_ones", "and", -1, true)
identity("or_zero", "or", 0, true)
identity("or_all_ones", "or", -1, true, -1)
identity("xor_zero", "xor", 0, true)
identity("shl_zero", "shl", 0, false)
identity("shr_zero", "shr", 0, false)

for name, spec in pairs({xor_self={"xor",0}, sub_self={"sub",0},
                         and_self={"and"}, or_self={"or"},
                         eq_self={"eq",1}, ne_self={"ne",0}, lt_self={"lt",0},
                         le_self={"le",1}, gt_self={"gt",0}, ge_self={"ge",1}}) do
    rules[name] = function(_, i)
        if i.op ~= spec[1] or not same(i.operands[1], i.operands[2]) then return false end
        return spec[2] ~= nil and constant(i, spec[2]) or copy(i, i.operands[1])
    end
end
for name, op in pairs({double_negation="neg", double_not="not"}) do
    rules[name] = function(s, i)
        if i.op ~= op then return false end
        local inner = s:definition(i.operands[1], op)
        return inner and copy(i, inner.operands[1]) or false
    end
end
rules.add_negation = function(s, i)
    if i.op ~= "add" then return false end
    for n=1,2 do
        local neg = s:definition(i.operands[n], "neg")
        if neg and same(neg.operands[1], i.operands[3-n]) then return constant(i, 0) end
    end
    return false
end
rules.subtract_negation = function(s, i)
    if i.op ~= "sub" then return false end
    local neg = s:definition(i.operands[2], "neg")
    return neg and rewrite(i, "add", {i.operands[1], neg.operands[1]}) or false
end
rules.select_true = function(s, i)
    return i.op == "select" and s:is(i.operands[1], 1, 1) and copy(i, i.operands[2]) or false
end
rules.select_false = function(s, i)
    return i.op == "select" and s:is(i.operands[1], 0, 1) and copy(i, i.operands[3]) or false
end
rules.select_identical = function(_, i)
    return i.op == "select" and same(i.operands[2], i.operands[3]) and copy(i, i.operands[2]) or false
end
rules.select_boolean_identity = function(s, i)
    return i.op == "select" and i.bits == 1 and s:is(i.operands[2], 1, 1)
        and s:is(i.operands[3], 0, 1) and copy(i, i.operands[1]) or false
end
rules.select_boolean_inversion = function(s, i)
    return i.op == "select" and i.bits == 1 and s:is(i.operands[2], 0, 1)
        and s:is(i.operands[3], 1, 1) and rewrite(i, "xor", {i.operands[1], literal(1,1)}) or false
end
rules.phi_constant = function(s, i)
    if i.op ~= "phi" then return false end
    -- Only literal incoming values: a constant instruction inside the loop
    -- must not be assumed defined on an external predecessor edge.
    local first = i.operands[1]
    if first.kind ~= "integer" then return false end
    for _, o in ipairs(i.operands) do if not same(first, o) then return false end end
    copy(i, first)
    -- Firestone requires all phis before non-phis. Move the replacement after
    -- remaining phis, keeping the stable ValueId and every CFG edge.
    local block = s.ctx.blocks[s.owners[i]]
    for n, inst in ipairs(block) do if inst == i then table.remove(block, n); break end end
    local n = 1
    while block[n] and block[n].op == "phi" do n = n+1 end
    table.insert(block, n, i)
    return true
end
rules.copy_propagation = function(s, i)
    return i.op == "copy" and s:replace_uses(i.result_id, i.operands[1]) or false
end
rules.constant_propagation = function(s, i)
    return i.op == "constant" and i.operands[1].kind == "integer"
        and s:replace_uses(i.result_id, i.operands[1]) or false
end

local commutative = {add=true, mul=true, ["and"]=true, ["or"]=true, xor=true, eq=true, ne=true}
local function operand_key(o)
    if o.kind == "value" then return "v" .. o.value end
    return "i" .. o.bits .. ":" .. normalize(o.integer, o.bits)
end
local function expression_key(i)
    local keys = {}
    for _, o in ipairs(i.operands) do keys[#keys+1] = operand_key(o) end
    if commutative[i.op] then table.sort(keys) end
    return i.op .. ":" .. i.bits .. ":" .. table.concat(keys, ",")
end
local function cse(ops)
    return function(s, i)
        if not ops[i.op] then return false end
        local key, owner = expression_key(i), s.owners[i]
        for _, candidate in ipairs(s.instructions) do
            if candidate ~= i and ops[candidate.op] and s:eligible(candidate)
                and s.ctx.dominators[owner][s.owners[candidate]]
                and (owner ~= s.owners[candidate] or s.positions[candidate] < s.positions[i])
                and expression_key(candidate) == key then
                return copy(i, {kind="value", value=candidate.result_id})
            end
        end
        return false
    end
end
rules.cse_integer_arithmetic = cse({add=true, sub=true, mul=true, neg=true})
rules.cse_integer_bitwise = cse({["and"]=true, ["or"]=true, xor=true, ["not"]=true})
rules.cse_integer_comparison = cse(comparison)
rules.cse_select = cse({select=true})

for _, op in ipairs({"add", "mul", "and", "or", "xor"}) do
    rules[op .. "_associate_constants"] = function(s, i)
        local inner_ref, b = s:split(i, op)
        if not inner_ref then return false end
        local inner = s:definition(inner_ref, op)
        if not inner then return false end
        local x, a = s:split(inner, op)
        if not x then return false end
        local combined
        if op == "add" then combined = a+b
        elseif op == "mul" then combined = a*b
        elseif op == "and" then combined = a&b
        elseif op == "or" then combined = a|b
        else combined = a~b end
        return rewrite(i, op, {x, literal(combined, i.bits)})
    end
end
for name, ops in pairs({and_absorption={"and","or"}, or_absorption={"or","and"}}) do
    rules[name] = function(s, i)
        if i.op ~= ops[1] then return false end
        for n=1,2 do
            local inner = s:definition(i.operands[n], ops[2])
            local other = i.operands[3-n]
            if inner and (same(inner.operands[1], other) or same(inner.operands[2], other)) then
                return copy(i, other)
            end
        end
        return false
    end
end
rules.xor_cancel = function(s, i)
    if i.op ~= "xor" then return false end
    for n=1,2 do
        local inner = s:definition(i.operands[n], "xor")
        if inner then for k=1,2 do
            if same(inner.operands[k], i.operands[3-n]) then return copy(i, inner.operands[3-k]) end
        end end
    end
    return false
end
rules.add_sub_cancel = function(s, i)
    if i.op ~= "sub" then return false end
    local inner = s:definition(i.operands[1], "add")
    if inner then for n=1,2 do
        if same(inner.operands[n], i.operands[2]) then return copy(i, inner.operands[3-n]) end
    end end
    return false
end
rules.sub_add_cancel = function(s, i)
    if i.op ~= "add" then return false end
    for n=1,2 do
        local inner = s:definition(i.operands[n], "sub")
        if inner and same(inner.operands[2], i.operands[3-n]) then return copy(i, inner.operands[1]) end
    end
    return false
end
rules.sub_sub_cancel = function(s, i)
    if i.op ~= "sub" then return false end
    local inner = s:definition(i.operands[2], "sub")
    return inner and same(inner.operands[1], i.operands[1]) and copy(i, inner.operands[2]) or false
end
for name, spec in pairs({and_complement={"and",0}, or_complement={"or",-1}}) do
    rules[name] = function(s, i)
        if i.op ~= spec[1] then return false end
        for n=1,2 do
            local inner = s:definition(i.operands[n], "not")
            if inner and same(inner.operands[1], i.operands[3-n]) then return constant(i, spec[2]) end
        end
        return false
    end
end

return function(name)
    local rule = assert(rules[name], "unknown local rewrite: " .. tostring(name))
    return function(ctx)
        if ctx.integer_semantics ~= "modular" then return false end
        assert(type(ctx.value_types) == "table", "local rewrites require value_types")
        local s, changed = context(ctx), false
        for _, inst in ipairs(s.instructions) do
            if s:eligible(inst) then changed = rule(s, inst) or changed end
        end
        return changed
    end
end
