LIB = asdl.lib.new "std.diagnostics"

function LIB.FaultPattern(code, format)
    return {code = code, format = format}
end

local function emit(severity, pattern, node, ...)
    local code, message
    if type(pattern) == "table" then
        code, message = pattern.code, string.format(pattern.format, ...)
    else
        message = tostring(pattern)
    end
    return asdl.diagnostic.emit {
        severity = severity, code = code, message = message,
        location = node and asdl.location(node) or nil
    }
end

function LIB.Error(pattern, node, ...) return emit("error", pattern, node, ...) end
function LIB.Warn(pattern, node, ...) return emit("warning", pattern, node, ...) end
function LIB.Inform(pattern, node, ...) return emit("info", pattern, node, ...) end
function LIB.Fatal(pattern, node, ...)
    LIB.Error(pattern, node, ...)
    error("ASDL semantic analysis aborted", 0)
end
function LIB.Checkpoint() return asdl.diagnostic.count("error") end
function LIB.FailedSince(mark) return LIB.Checkpoint() > mark end

asdl.signlib(LIB)
return LIB
