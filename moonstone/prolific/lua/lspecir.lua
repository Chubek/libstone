-- SpecIR v1: declarative profile-driven speculation, not executable guards.
-- This library validates data and dispatches translators; it installs no checks
-- in a compiler/VM and makes no proof claims. Compatible with Lua 5.3+.
local M = {version = 1}
local function check(ok, message)
    if not ok then error('SpecIR: ' .. message, 3) end
end
local function plain(t)
    check(type(t) == 'table' and getmetatable(t) == nil, 'expected a plain table')
end
local function keys(t, allowed)
    plain(t)
    for key in pairs(t) do check(allowed[key], 'unknown field ' .. tostring(key)) end
end
local function fields(s)
    local result = {}
    for word in s:gmatch('%S+') do result[word] = true end
    return result
end
local function str(s, nonempty)
    check(type(s) == 'string' and #s <= 1048576 and (not nonempty or #s > 0), 'invalid string')
    check(utf8.len(s) ~= nil, 'invalid UTF-8 string')
end
-- Counts/identities are decimal strings. Never round them through Lua doubles.
local function unsigned(s)
    str(s, true)
    check(s == '0' or s:match('^[1-9][0-9]*$'), 'noncanonical unsigned integer')
    check(#s < 20 or (#s == 20 and s <= '18446744073709551615'), 'unsigned integer overflow')
end
local function signed(s)
    str(s, true)
    local negative = s:sub(1, 1) == '-'
    local magnitude = negative and s:sub(2) or s
    unsigned(magnitude)
    check(not negative or magnitude ~= '0', 'negative zero')
    local bound = negative and '9223372036854775808' or '9223372036854775807'
    check(#magnitude < #bound or (#magnitude == #bound and magnitude <= bound), 'signed integer overflow')
end
local function le(a, b)
    return #a < #b or (#a == #b and a <= b)
end
local function sle(a, b)
    local an, bn = a:sub(1,1) == '-', b:sub(1,1) == '-'
    if an ~= bn then return an end
    if an then return le(b:sub(2), a:sub(2)) end
    return le(a, b)
end
local function count(s)
    unsigned(s)
    check(le(s, '1000000'), 'profile count exceeds v1 limit')
    return tonumber(s) -- exact: bounded above, even on a double-only Lua
end
local function array(t, limit)
    plain(t)
    local n = #t
    check(n <= limit, 'array budget exhausted')
    local seen = 0
    for k in pairs(t) do
        check(type(k) == 'number' and k % 1 == 0 and k >= 1 and k <= n, 'expected dense array')
        seen = seen + 1
    end
    check(seen == n, 'array contains holes')
    return n
end
local categories = fields('type branch value call interoperation system cost lowering allocation exception custom')
local numerics = fields('value cost allocation')
local schemas = {
    profile = fields('op id category domain site context dimension unit location samples omitted first last numeric buckets minimum maximum sum sum_overflow'),
    guard = fields('op id profile predicate expected minimum maximum phase on_miss matches samples evidence enforced'),
    advice = fields('op id profile guard code message confidence'),
    warning = fields('op id profile code message'),
}
local rules = {
    type_is = {'type', 'before_use', 'generic_path'},
    branch_is = {'branch', 'after_condition', 'cold_path'},
    value_is = {'value', 'before_use', 'generic_path'},
    value_in_range = {'value', 'before_use', 'generic_path', true},
    callee_is = {'call', 'before_dispatch', 'generic_dispatch'},
    interop_contract_is = {'interoperation', 'before_boundary', 'generic_boundary'},
    system_outcome_is = {'system', 'after_effect', 'reprofile'},
    cost_in_range = {'cost', 'after_measurement', 'reprofile', true},
    lowering_context_is = {'lowering', 'before_lowering', 'generic_lowering'},
    allocation_is = {'allocation', 'before_allocation', 'generic_allocator'},
    allocation_in_range = {'allocation', 'before_allocation', 'generic_allocator', true},
    exception_is = {'exception', 'after_exception', 'generic_handler'},
    custom_is = {'custom', 'before_use', 'generic_path'},
}
local function instruction(i)
    plain(i)
    local schema = schemas[i.op]
    check(schema ~= nil, 'unknown instruction ' .. tostring(i.op))
    keys(i, schema)
    str(i.id, true)
    if i.op == 'profile' then
        check(categories[i.category], 'unknown profile category')
        check(i.domain == 'compiler' or i.domain == 'vm' or i.domain == 'runtime', 'unknown domain')
        str(i.site, true); str(i.context, true); str(i.dimension, true); str(i.unit)
        local n, omitted = count(i.samples), count(i.omitted)
        check(n > 0 and omitted <= n, 'invalid profile counts')
        unsigned(i.first); unsigned(i.last)
        check(i.first ~= '0' and le(i.first, i.last), 'invalid evidence sequence range')
        check(i.numeric == (numerics[i.category] == true), 'wrong numeric profile flag')
        keys(i.location, fields('file ir_id line column'))
        str(i.location.file); str(i.location.ir_id)
        for _, k in ipairs({'line', 'column'}) do
            local v = i.location[k]
            check(type(v) == 'number' and v % 1 == 0 and v >= 0 and v <= 4294967295, 'invalid source location')
        end
        if i.numeric then
            str(i.unit, true); signed(i.minimum); signed(i.maximum)
            check(sle(i.minimum, i.maximum), 'invalid numeric range')
            check(type(i.sum_overflow) == 'boolean', 'missing sum overflow flag')
            if i.sum_overflow then check(i.sum == '', 'overflowed sum must be unavailable')
            else signed(i.sum) end
            if i.category ~= 'value' then check(sle('0', i.minimum), 'negative cost/allocation') end
        else
            check(i.unit == '' and i.minimum == nil and i.maximum == nil and i.sum == nil and
                  i.sum_overflow == nil, 'categorical profile has numeric fields')
        end
        array(i.buckets, 4096)
        local total, seen = omitted, {}
        local observed_min, observed_max
        for _, b in ipairs(i.buckets) do
            keys(b, fields('value count')); str(b.value, true)
            check(not seen[b.value], 'duplicate bucket'); seen[b.value] = true
            local c = count(b.count); check(c > 0, 'empty bucket'); total = total + c
            if i.numeric then
                signed(b.value)
                check(sle(i.minimum, b.value) and sle(b.value, i.maximum), 'bucket outside numeric range')
                if not observed_min or sle(b.value, observed_min) then observed_min = b.value end
                if not observed_max or sle(observed_max, b.value) then observed_max = b.value end
            elseif i.category == 'branch' then
                check(b.value == 'true' or b.value == 'false', 'invalid branch outcome')
            end
        end
        check(total == n, 'histogram does not account for all samples')
        if i.numeric and omitted == 0 then
            check(i.minimum == observed_min and i.maximum == observed_max, 'range not supported by histogram')
        end
    elseif i.op == 'guard' then
        str(i.profile, true)
        check(rules[i.predicate] ~= nil, 'unknown predicate')
        str(i.expected); str(i.minimum); str(i.maximum)
        local r = rules[i.predicate]
        check(i.phase == r[2] and i.on_miss == r[3], 'invalid guard phase or fallback')
        if r[4] then
            check(i.expected == '', 'range guard has equality operand')
            signed(i.minimum); signed(i.maximum)
            check(sle(i.minimum, i.maximum), 'reversed guard range')
        else
            str(i.expected, true)
            check(i.minimum == '' and i.maximum == '', 'equality guard has range operands')
        end
        local matches, samples = count(i.matches), count(i.samples)
        check(matches > 0 and matches <= samples, 'invalid guard counts')
        check(i.evidence == 'observed' and i.enforced == false, 'guards are unenforced observations')
    else
        str(i.profile, i.op == 'advice'); str(i.code, true); str(i.message, true)
        if i.op == 'advice' then
            str(i.guard, true); check(i.confidence == 'heuristic', 'advice is heuristic')
        end
    end
    return i
end
-- Constructors copy plain data so retaining the input table cannot mutate IR.
local function copy(t, depth)
    depth = depth or 0
    check(depth < 16, 'nested/cyclic data exceeds depth budget')
    if type(t) ~= 'table' then
        check(type(t) == 'string' or type(t) == 'number' or type(t) == 'boolean', 'non-data value')
        return t
    end
    plain(t)
    local result, n = {}, 0
    for k, v in pairs(t) do
        n = n + 1; check(n <= 1000000, 'table budget exhausted')
        check(type(k) == 'number' or type(k) == 'string', 'non-data key')
        result[k] = copy(v, depth + 1)
    end
    return result
end
for op in pairs(schemas) do
    local opcode = op
    M[opcode] = function(data)
        local i = copy(data)
        check(i.op == nil or i.op == opcode, 'constructor opcode mismatch')
        i.op = opcode
        return instruction(i)
    end
end
local function validate(m)
    keys(m, fields('version source_schema created_unix_ns complete rejected open_spans minimum_samples dominance_permille instructions'))
    check(m.version == 1 and m.source_schema == 'moonstone.prolific/1', 'unsupported module version/schema')
    unsigned(m.created_unix_ns); unsigned(m.rejected); unsigned(m.open_spans)
    check(type(m.complete) == 'boolean' and m.complete == (m.rejected == '0' and m.open_spans == '0'), 'inconsistent collection quality')
    local minimum = count(m.minimum_samples); check(minimum > 0, 'invalid minimum samples')
    local dominance = m.dominance_permille
    check(type(dominance) == 'number' and dominance % 1 == 0 and dominance >= 501 and dominance <= 1000, 'invalid dominance threshold')
    array(m.instructions, 1000000)
    local ids, profiles, guards = {}, {}, {}
    for _, i in ipairs(m.instructions) do
        instruction(i)
        check(not ids[i.id], 'duplicate instruction ID'); ids[i.id] = true
        if i.op == 'profile' then profiles[i.id] = i
        elseif i.op == 'warning' then
            check(i.profile == '' or profiles[i.profile], 'warning references missing/forward profile')
        else
            local p = profiles[i.profile]
            check(p ~= nil, 'missing/forward evidence profile')
            check(m.complete and p.omitted == '0' and count(p.samples) >= minimum, 'inadequate evidence for speculation')
            if i.op == 'guard' then
                local r = rules[i.predicate]
                check(p.category == r[1] and i.samples == p.samples, 'guard evidence mismatch')
                if r[4] then
                    check(i.minimum == p.minimum and i.maximum == p.maximum and i.matches == p.samples, 'range guard evidence mismatch')
                else
                    local matches
                    for _, b in ipairs(p.buckets) do if b.value == i.expected then matches = b.count end end
                    check(matches == i.matches and count(i.matches) * 1000 >= count(p.samples) * dominance, 'equality guard lacks dominant evidence')
                end
                guards[i.id] = i
            else
                local g = guards[i.guard]
                check(g ~= nil and g.profile == i.profile, 'advice requires a preceding matching guard')
            end
        end
    end
    return m
end
function M.module(data) return validate(copy(data)) end
function M.validate(data) validate(data); return true end

-- Preflight all handlers before any call. Handlers receive detached records;
-- modifications cannot change later dispatch or the original module. Returning
-- nil is valid; results remain dense via one result envelope per instruction.
function M.translator(handlers)
    keys(handlers, fields('profile guard advice warning'))
    local captured = {}
    for k, fn in pairs(handlers) do
        check(type(fn) == 'function', 'translator handler must be a function')
        captured[k] = fn
    end
    return {translate = function(_, module, context)
        local plan = validate(copy(module))
        for _, i in ipairs(plan.instructions) do
            check(captured[i.op] ~= nil, 'missing translator handler: ' .. i.op)
        end
        local results = {}
        for n, i in ipairs(plan.instructions) do
            local value = captured[i.op](copy(i), context)
            results[n] = {id = i.id, op = i.op, value = value}
        end
        return results
    end}
end
return M
