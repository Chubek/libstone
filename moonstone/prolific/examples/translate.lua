-- LUA_PATH='moonstone/prolific/lua/?.lua;;' prolific-lua translate.lua profile.spec.lua
-- Emits machine-readable TSV for a downstream consumer, preserving exact IDs
-- and integers. This example translates information; it installs no guards.
local ir = require('lspecir')
local program = assert(loadfile(assert(arg[1], 'expected SpecIR file')))()
local function cell(value)
    return tostring(value):gsub('\\', '\\\\'):gsub('\t', '\\t'):gsub('\n', '\\n'):gsub('\r', '\\r'):gsub('%z', '\\0')
end
local function row(...)
    local values = {...}
    for i, value in ipairs(values) do values[i] = cell(value) end
    io.write(table.concat(values, '\t'), '\n')
end
local profiles = {}
local translator = ir.translator {
    profile = function(p)
        profiles[p.id] = p
        row('profile', p.id, p.category, p.site, p.context, p.dimension, p.samples)
    end,
    guard = function(g)
        local p = profiles[g.profile]
        row('guard', g.id, p.site, p.context, g.predicate, g.expected,
            g.minimum, g.maximum, p.unit, g.phase, g.on_miss, g.matches, g.samples)
    end,
    advice = function(a)
        row('advice', a.id, a.profile, a.guard, a.code, a.confidence, a.message)
    end,
    warning = function(w)
        row('warning', w.id, w.profile, w.code, w.message)
    end,
}
translator:translate(program)
