LIB = asdl.lib.new "std.diagnostics"

local enabled = {error = true, warning = true, info = true}
local groups, names = {}, {}
local suppressed_errors = 0

local function check_severity(severity)
    assert(enabled[severity] ~= nil, "expected diagnostic severity: error, warning, or info")
end

local function check_label(value, label)
    assert(type(value) == "string" and value ~= "", "expected nonempty diagnostic " .. label)
end

local function is_enabled(severity, pattern)
    if type(pattern) == "table" then
        local by_name = names[pattern.name]
        if by_name ~= nil then return by_name end
        local by_group = groups[pattern.group]
        if by_group ~= nil then return by_group end
    end
    return enabled[severity]
end

function LIB.IsEnabled(severity, pattern)
    check_severity(severity)
    return is_enabled(severity, pattern)
end

local function set_enabled(selector, value)
    if selector == nil then
        for name in pairs(enabled) do enabled[name] = value end
        groups, names = {}, {}
    elseif type(selector) == "table" then
        local key, label = next(selector)
        assert((key == "group" or key == "name") and next(selector, key) == nil,
            "expected a diagnostic selector with exactly one group or name")
        check_label(label, key)
        local overrides = key == "group" and groups or names
        overrides[label] = value
    else
        check_severity(selector)
        enabled[selector] = value
    end
end

function LIB.Enable(selector) set_enabled(selector, true) end
function LIB.Disable(selector) set_enabled(selector, false) end

function LIB.FaultPattern(code, format, metadata)
    local pattern = {code = code, format = format}
    if metadata ~= nil then
        assert(type(metadata) == "table", "expected diagnostic metadata")
        for key, value in pairs(metadata) do
            assert(key == "name" or key == "group", "expected diagnostic metadata keys: name or group")
            check_label(value, key)
            pattern[key] = value
        end
    end
    return pattern
end

local function emit(severity, pattern, node, ...)
    if not is_enabled(severity, pattern) then
        if severity == "error" then suppressed_errors = suppressed_errors + 1 end
        return nil
    end
    local code, message, name, group
    if type(pattern) == "table" then
        code, message = pattern.code, string.format(pattern.format, ...)
        name, group = pattern.name, pattern.group
    else
        message = tostring(pattern)
    end
    return asdl.diagnostic.emit {
        severity = severity, code = code, message = message, name = name, group = group,
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
function LIB.Checkpoint() return asdl.diagnostic.count("error") + suppressed_errors end
function LIB.FailedSince(mark) return LIB.Checkpoint() > mark end

asdl.signlib(LIB)
return LIB
