LIB = asdl.lib.new "std.control"

local Control = {}
function Control.enter(parent, kind, owner, ctx)
    ctx.next_control = (ctx.next_control or 0) + 1
    local frame = {parent = parent, kind = kind, id = ctx.next_control}
    asdl.annotate(owner, "control-id", frame.id)
    return frame
end

local targets = {
    Return = {method=true, lambda=true},
    Break  = {loop=true, block=true, lambda=true},
    Next   = {loop=true, block=true, lambda=true},
    Retry  = {rescue=true},
    Yield  = {method=true},
    Super  = {method=true},
}

function Control.target(frame, operation, policy)
    local wanted = assert((policy and policy.targets or targets)[operation], "unknown control operation")
    while frame do
        if wanted[frame.kind] then return frame end
        if policy and policy.barrier then
            if policy.barrier(frame, operation) then return nil end
        elseif frame.kind == "method" or frame.kind == "class" or frame.kind == "top" then
            return nil
        end
        -- retry does not cross a closure boundary in MiniRuby.
        if not policy and operation == "Retry" and (frame.kind == "block" or frame.kind == "lambda") then
            return nil
        end
        frame = frame.parent
    end
end

LIB.Control = Control
asdl.signlib(LIB)
return LIB
