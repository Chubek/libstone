-- Pure Lua synthesis grammar and bounded expression enumerator. No native dependency.
local M = {}
local function serialize(expr)
  if type(expr) == 'string' then return expr end
  return '(' .. expr.op .. ' ' .. serialize(expr.left) .. ' ' .. serialize(expr.right) .. ')'
end
function M.define(grammar)
  assert(type(grammar) == 'table', 'grammar must be table')
  assert(type(grammar.variables) == 'table', 'variables required')
  assert(type(grammar.constants) == 'table', 'constants required')
  grammar.operations = grammar.operations or {'+', '-', '*'}
  return grammar
end
local function eval(e, variables)
  if type(e) == 'string' then
    if variables[e] ~= nil then return variables[e] end
    return tonumber(e)
  end
  local a, b = eval(e.left, variables), eval(e.right, variables)
  if not a or not b then return nil end
  if e.op == '+' then return a+b elseif e.op == '-' then return a-b elseif e.op == '*' then return a*b end
end
M.evaluate = eval
M.serialize = serialize
function M.solve(grammar, examples, opts)
  grammar = M.define(grammar)
  opts = opts or {}
  local max_size = opts.max_size or 5
  local max_candidates = opts.max_candidates or 50000
  assert(max_size >= 1 and max_size <= 10)
  local levels, seen, evaluated = {}, {}, 0
  local function try(e, size)
    local key = serialize(e)
    if seen[key] then return nil end
    seen[key] = true
    levels[size][#levels[size]+1] = e
    evaluated = evaluated + 1
    if evaluated > max_candidates then return nil end
    for _,example in ipairs(examples) do
      if eval(e, example.inputs) ~= example.output then return nil end
    end
    return e
  end
  for size=1,max_size do
    levels[size] = {}
    if size == 1 then
      for _,name in ipairs(grammar.variables) do local e=try(name,size);if e then return {status='found',program=e,checked=evaluated} end end
      for _,number in ipairs(grammar.constants) do local e=try(tostring(number),size);if e then return {status='found',program=e,checked=evaluated} end end
    else
      for left=1,size-2 do
        local right = size-left-1
        for _,a in ipairs(levels[left]) do for _,b in ipairs(levels[right]) do
          for _,op in ipairs(grammar.operations) do
            local e=try({op=op,left=a,right=b},size)
            if e then return {status='found',program=e,checked=evaluated} end
            if evaluated>max_candidates then return {status='budget',checked=evaluated} end
          end
        end end
      end
    end
  end
  return {status='exhausted_within_bound',checked=evaluated}
end
return M
