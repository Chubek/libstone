-- The kernel catalog and the SSA host adapter are separate facilities.
local M = {}

function M.new_environment()
    local kernel = {}
    function kernel.define(metadata)
        assert(kernel.metadata == nil, "only one kernel declaration is allowed per file")
        assert(type(metadata) == "table", "kernel metadata must be a table")
        local fields = {name=true, ir=true, phase=true, requires=true,
                        preserves=true, documentation=true}
        for key in pairs(metadata) do
            assert(fields[key], "unknown kernel field: " .. tostring(key))
        end
        kernel.metadata = metadata
    end
    -- Each file has independent globals and a fresh kernel slot. Standard Lua
    -- libraries are available: this is an embedding environment, not a sandbox.
    local api = setmetatable({kernel=kernel}, {__index=M})
    return setmetatable({lrewrite=api}, {__index=_G})
end

local function iterator(items)
    local i = 0
    return function() i = i + 1; return items[i] end
end

local function is_ref(operand, id)
    return operand.kind == "value" and operand.value == id
end
local function is_int(operand, value, bits)
    return operand.kind == "integer" and operand.integer == value
       and operand.bits == bits
end
local function ref(value) return {kind="value", value=value} end
local function integer(value, bits)
    return {kind="integer", integer=value, bits=bits}
end

-- Convert a Firestone snapshot to the object protocol used by Lua kernels.
-- Mutations affect the snapshot only. The native adapter validates and commits.
function M.ssa_context(ctx)
    local definitions, values, replacements = {}, {}, {}
    local value_methods, instruction_methods, loop_methods = {}, {}, {}
    local function value(id, bits, kind, name)
        if not values[id] then
            values[id] = setmetatable({id=id, name="v" .. id,
                                      type={kind=kind or "integer", bits=bits, name=name or ""}},
                                     {__index=value_methods})
        end
        return values[id]
    end
    local function attach(inst, block)
        inst.block = block
        if inst.result_id then
            inst.result = value(inst.result_id, inst.bits, inst.kind, inst.type_name)
            definitions[inst.result_id] = inst
        end
        setmetatable(inst, {__index=instruction_methods})
    end
    for id, block in pairs(ctx.blocks) do
        for _, inst in ipairs(block) do attach(inst, id) end
    end
    function value_methods:replace_uses_with(replacement)
        assert(values[replacement.id] == replacement, "foreign replacement value")
        assert(self.type.kind == replacement.type.kind and self.type.bits == replacement.type.bits
               and self.type.name == replacement.type.name, "replacement type mismatch")
        replacements[self.id] = replacement.id
        for _, block in pairs(ctx.blocks) do
            for _, inst in ipairs(block) do
                for n, operand in ipairs(inst.operands) do
                    if is_ref(operand, self.id) then inst.operands[n] = ref(replacement.id) end
                end
            end
        end
    end
    local function find_induction(loop)
        -- This version intentionally recognizes only a zero-based, unit-step
        -- integer induction. All outside incoming values must be literal zero.
        for _, phi in ipairs(ctx.blocks[loop.header]) do
            if phi.op ~= "phi" then break end
            if phi.kind == "integer" and (phi.type_name or "") == ""
                and phi.bits >= 2 and phi.bits <= 64 then
                local good, outside, backedges = true, 0, 0
                for n, operand in ipairs(phi.operands) do
                    local predecessor = phi.incoming[n]
                    if loop.members[predecessor] then
                        backedges = backedges + 1
                        local update = operand.kind == "value" and definitions[operand.value]
                        if not (loop.latches[predecessor] and update and update.op == "add"
                            and update.kind == "integer" and update.bits == phi.bits
                            and ctx.dominators[predecessor][update.block]
                            and ((is_ref(update.operands[1], phi.result_id)
                                  and is_int(update.operands[2], 1, phi.bits))
                              or (is_ref(update.operands[2], phi.result_id)
                                  and is_int(update.operands[1], 1, phi.bits)))) then
                            good = false
                        end
                    else
                        outside = outside + 1
                        if not is_int(operand, 0, phi.bits) then good = false end
                    end
                end
                if good and outside > 0 and backedges > 0 then return phi.result end
            end
        end
    end
    function loop_methods:induction_variable()
        self.induction = self.induction or find_induction(self)
        return self.induction
    end
    function loop_methods:instructions()
        local snapshot = {}
        for _, id in ipairs(self.blocks) do
            for _, inst in ipairs(ctx.blocks[id]) do snapshot[#snapshot+1] = inst end
        end
        return iterator(snapshot)
    end
    function instruction_methods:match_induction_times_constant(induction)
        if self.op ~= "mul" or self.kind ~= "integer" or self.bits ~= induction.type.bits then return end
        for n=1,2 do
            local constant = self.operands[3-n]
            if is_ref(self.operands[n], induction.id) and constant.kind == "integer"
                and constant.bits == self.bits then return induction, constant.integer end
        end
    end
    function ctx:can_strength_reduce(loop, inst, induction, scale)
        if self.integer_semantics ~= "modular" or loop:induction_variable() ~= induction
            or inst.op ~= "mul" or not loop.members[inst.block]
            or not ctx.dominators[inst.block][loop.header] then return false end
        local iv, actual = inst:match_induction_times_constant(induction)
        return iv ~= nil and actual == scale and math.type(scale) == "integer" and scale > 1
    end
    local function new_id(bits)
        local id = ctx.next_value
        ctx.next_value = id + 1
        ctx.new_types[#ctx.new_types+1] = bits
        return id
    end
    function loop_methods:create_recurrence(spec)
        local induction = assert(self:induction_variable(), "no supported induction")
        assert(spec.initial == 0 and spec.type.bits == induction.type.bits,
               "unsupported recurrence type or initial value")
        assert(ctx.integer_semantics == "modular" and math.type(spec.step) == "integer"
               and spec.step > 1, "unsupported recurrence step or arithmetic semantics")
        local original = definitions[induction.id]
        local id = new_id(spec.type.bits)
        local phi = {op="phi", result_id=id, kind="integer", bits=spec.type.bits,
                     operands={}, incoming={}, targets={}, annotation=spec.name}
        for n, predecessor in ipairs(original.incoming) do
            phi.incoming[n] = predecessor
            if self.members[predecessor] then
                local update_id = new_id(spec.type.bits)
                phi.operands[n] = ref(update_id)
            else phi.operands[n] = integer(0, spec.type.bits) end
        end
        attach(phi, self.header)
        table.insert(ctx.blocks[self.header], 1, phi)
        local recurrence = {value=value(id, spec.type.bits)}
        function recurrence:advance_on_backedges()
            assert(not self.advanced, "recurrence already advanced")
            self.advanced = true
            for n, predecessor in ipairs(phi.incoming) do
                if phi.operands[n].kind == "value" then
                    local update = {op="add", result_id=phi.operands[n].value,
                                    kind="integer", bits=spec.type.bits,
                                    operands={ref(id), integer(spec.step, spec.type.bits)},
                                    targets={}, incoming={}, annotation=spec.name .. "_next"}
                    attach(update, predecessor)
                    local block = ctx.blocks[predecessor]
                    table.insert(block, #block, update) -- immediately before terminator
                end
            end
        end
        return recurrence
    end
    function ctx:erase(inst)
        -- Firestone reserves stable ValueIds and requires each to retain a
        -- definition. Keep a dead copy instead of leaving an undefined ID.
        -- The recurrence replacement has already rewritten every live use.
        assert(inst.op == "mul", "SSA erase supports replaced multiplications only")
        for _, block in pairs(ctx.blocks) do
            for _, use in ipairs(block) do
                for _, operand in ipairs(use.operands) do
                    assert(not is_ref(operand, inst.result_id), "cannot erase a live definition")
                end
            end
        end
        local replacement = replacements[inst.result_id]
        inst.op, inst.operands = "copy", {ref(assert(replacement, "missing replacement recurrence"))}
    end
    for _, loop in ipairs(ctx.loop_list) do
        setmetatable(loop, {__index=loop_methods})
    end
    ctx.loops = {all=function() return iterator(ctx.loop_list) end}
    return ctx
end

return M
