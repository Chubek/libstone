-- Declarative SymAS document collector. Semantic validation stays in the C++
-- compiler. Deterministic JSON needs no external Lua module or subprocess.
local M = {}
local array = {}
local function encode(value, seen)
    local kind = type(value)
    if kind == 'string' then
        return '"' .. value:gsub('[%z\1-\31\\"]', function(c)
            if c == '"' then return '\\"' end
            if c == '\\' then return '\\\\' end
            return string.format('\\u%04x', string.byte(c))
        end) .. '"'
    elseif kind == 'boolean' then return value and 'true' or 'false'
    elseif kind == 'number' then
        assert(value == value and value ~= math.huge and value ~= -math.huge, 'non-finite JSON number')
        return tostring(value)
    end
    assert(kind == 'table', 'unsupported JSON value: ' .. kind)
    assert(not seen[value], 'cyclic specification table')
    seen[value] = true
    local parts = {}
    if getmetatable(value) == array or #value > 0 then
        for key in pairs(value) do
            assert(type(key) == 'number' and key >= 1 and key <= #value and key % 1 == 0, 'mixed/sparse JSON array')
        end
        for i, item in ipairs(value) do parts[i] = encode(item, seen) end
        seen[value] = nil
        return '[' .. table.concat(parts, ',') .. ']'
    end
    local keys = {}
    for key in pairs(value) do assert(type(key) == 'string', 'JSON object key must be a string'); keys[#keys + 1] = key end
    table.sort(keys)
    for _, key in ipairs(keys) do parts[#parts + 1] = encode(key, seen) .. ':' .. encode(value[key], seen) end
    seen[value] = nil
    return '{' .. table.concat(parts, ',') .. '}'
end
function M.new()
    local document, api = {schema = 1}, {}
    for singular, plural in pairs({target='targets', feature='features', register='registers',
        mnemonic='mnemonics', encoder='encoders', form='forms', syntax='syntaxes',
        section='sections', linkage='linkages', reloc='relocs', debug='debug'}) do
        local records = setmetatable({}, array)
        document[plural] = records
        api[singular] = function(record)
            assert(type(record) == 'table', singular .. ' expects a record')
            if singular == 'reloc' then
                record.addend = record.addend or 'explicit'
                record.overflow = record.overflow or 'error'
            elseif singular == 'section' then record.content = record.content or 'unknown'
            elseif singular == 'linkage' then
                record.visibility = record.visibility or 'default'
                record.symtype = record.symtype or 'none'
            elseif singular == 'form' then
                for _, operand in ipairs(record.operands or {}) do operand.access = operand.access or 'read' end
            end
            -- Mark known sequence fields, including empty operand lists.
            for _, key in ipairs({'features','operands','flags','aliases','required','forbidden','syntaxes'}) do
                if type(record[key]) == 'table' then setmetatable(record[key], array) end
            end
            records[#records + 1] = record
        end
    end
    api.emit = function() return encode(document, {}) end
    return api
end
return M
