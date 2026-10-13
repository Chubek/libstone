lrewrite.kernel.define {
    name = "strength_reduction",
    ir = "ssa",
    phase = "loop_optimization",

    documentation = [[Replace zero-based, unit-step integer induction times a
constant greater than one with an SSA recurrence. Requires explicit modular
integer semantics, valid SSA, natural loops and dominance. Nonzero initial
values, nonunit steps and unknown/trapping overflow semantics are unsupported.
The recurrence starts at zero and advances on every backedge. CFG is preserved;
def-use information changes. Profitability depends on the target cost model.]],

    requires = {
        "loop_info",
        "def_use",
        "dominators"
    },

    preserves = {
        "cfg"
    }
}

lrewrite.kernel.run = function(ctx)
    local changed = false

    for loop in ctx.loops:all() do
        local induction = loop:induction_variable()

        if not induction then
            goto continue
        end

        -- Find multiplication by a constant:
        -- t = induction * constant
        for inst in loop:instructions() do
            if inst.op == "mul" then
                local iv, scale =
                    inst:match_induction_times_constant(
                        induction
                    )

                if iv and scale and scale > 1 then
                    -- Check that the recurrence is legal
                    -- for the integer type and overflow rules.
                    if ctx:can_strength_reduce(
                        loop, inst, induction, scale
                    ) then
                        local recurrence =
                            loop:create_recurrence({
                                name = inst.result.name
                                    .. "_reduced",
                                type = inst.result.type,
                                initial = 0,
                                step = scale
                            })

                        -- Replace uses of i * scale
                        -- with the recurrence value.
                        inst.result:replace_uses_with(
                            recurrence.value
                        )

                        -- Update the recurrence on each
                        -- loop backedge.
                        recurrence:advance_on_backedges()

                        ctx:erase(inst)
                        changed = true
                    end
                end
            end
        end

        ::continue::
    end

    return changed
end
