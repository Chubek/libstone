LIB = asdl.lib.derive "std/diagnostics.lua"

local messages = {
    E101 = "Duplicate parameter or block-local '%s'",
    E102 = "Invalid position or repetition of parameter '%s'",
    E103 = "Duplicate keyword argument '%s'",
    E104 = "Argument '%s' follows a block-pass argument",
    E105 = "An attached block and a block-pass argument cannot coexist",
    E106 = "Positional argument follows a keyword argument",
    E201 = "%s has no enclosing control target",
    E301 = "Unknown superclass '%s'",
    E302 = "Superclass mismatch when reopening '%s'",
    E303 = "Class '%s' cannot inherit from itself",
    E304 = "Class '%s' is reserved by the prelude",
    W301 = "Method '%s' redefines an earlier method in '%s'",
}

local function report(emit, code, node, ...)
    return emit(LIB.FaultPattern(code, assert(messages[code])), node, ...)
end
function LIB.RubyError(code, node, ...) return report(LIB.Error, code, node, ...) end
function LIB.RubyWarn(code, node, ...) return report(LIB.Warn, code, node, ...) end

asdl.signlib(LIB)
return LIB
