package.path = arg[1] .. '/?.lua;' .. package.path
local ir = require('lspecir')
local module = dofile(arg[2])
assert(ir.validate(module) and not _G.PWNED)
local categories, profiles, guards, advice, warnings = {}, {}, {}, 0, 0
local handlers = {
    profile = function(i, context)
        assert(context == 'test')
        categories[i.category] = true
        profiles[i.id] = i
        if i.category == 'value' and i.site ~= 'signed-range' then
            assert(i.maximum == '9223372036854775807' and i.sum_overflow and i.sum == '')
        end
        if i.site == 'signed-range' then
            assert(i.minimum == '-9223372036854775808' and i.maximum == '9223372036854775807')
            assert(i.sum == '-16' and not i.sum_overflow)
        end
        if i.category == 'custom' then
            assert(i.buckets[1].value:find('_G.PWNED = true', 1, true))
            assert(i.buckets[1].value:find('\0', 1, true))
        end
        return 'profile'
    end,
    guard = function(i)
        assert(profiles[i.profile] and i.enforced == false)
        guards[i.id] = i
        if profiles[i.profile].category == 'system' then
            assert(i.phase == 'after_effect' and i.on_miss == 'reprofile')
        end
        return 'guard'
    end,
    advice = function(i)
        assert(guards[i.guard] and i.confidence == 'heuristic')
        advice = advice + 1
        return 'advice'
    end,
    warning = function(i) warnings = warnings + 1 end,
}
local result = ir.translator(handlers):translate(module, 'test')
assert(#result == #module.instructions and advice == 12 and warnings >= 8)
for _, category in ipairs({'type','branch','value','call','interoperation','system','cost','lowering','allocation','exception','custom'}) do
    assert(categories[category])
end
local function clone(t)
    if type(t) ~= 'table' then return t end
    local c = {}; for k,v in pairs(t) do c[k] = clone(v) end; return c
end
local function fails(fn, contains)
    local ok, reason = pcall(fn)
    assert(not ok and tostring(reason):find(contains, 1, true), tostring(reason))
end
local calls = 0
fails(function() ir.translator{profile=function() calls = calls + 1 end}:translate(module) end, 'missing translator handler')
assert(calls == 0) -- preflight does not partially translate
local function first(m, op)
    for _, i in ipairs(m.instructions) do if i.op == op then return i end end
end
for _, mutate in ipairs({
    function(m) m.version = 2 end,
    function(m) m.instructions[1].op = 'execute' end,
    function(m) first(m, 'advice').guard = 'missing' end,
    function(m) first(m, 'guard').enforced = true end,
    function(m) first(m, 'guard').matches = '999999' end,
    function(m) first(m, 'guard').phase = 'before_effect' end,
    function(m) first(m, 'profile').samples = '01' end,
    function(m) first(m, 'profile').samples = '18446744073709551616' end,
    function(m) first(m, 'profile').buckets[1].count = '0' end,
    function(m) first(m, 'profile').location.line = -1 end,
    function(m) first(m, 'profile').site = '\255' end,
    function(m)
        for _, p in ipairs(m.instructions) do
            if p.op == 'profile' and p.category == 'allocation' then p.maximum = '99999'; break end
        end
    end,
    function(m) m.instructions[2].id = m.instructions[1].id end,
    function(m) m.rejected = '1'; m.complete = false end,
    function(m) m.instructions.extra = 'bad' end,
    function(m) m.unrecognized = true end,
}) do
    local m = clone(module); mutate(m)
    fails(function() ir.translator(handlers):translate(m) end, 'SpecIR:')
end
local thrown = clone(handlers) -- functions are kept by the test's copier
thrown.profile = function() error('translator failure') end
fails(function() ir.translator(thrown):translate(module) end, 'translator failure')
local detached = ir.module(module)
module.instructions[1].site = 'mutated'
assert(detached.instructions[1].site ~= 'mutated')
local cyclic = {}; cyclic.self = cyclic
fails(function() ir.module(cyclic) end, 'depth budget')
assert(ir.validate(dofile(arg[3]))) -- incomplete module must contain warnings/evidence only
local incomplete = dofile(arg[3])
for _, i in ipairs(incomplete.instructions) do assert(i.op ~= 'guard' and i.op ~= 'advice') end
print('SpecIR translation verified')
