-- VMWeave: a declarative virtual-machine description DSL.
--
-- A description declares immutable structure (name, execution model, cell type,
-- state fields, instruction words) plus authoritative C semantics per word.
-- VMWeave validates the declaration and emits a deterministic, self-contained
-- C99 translation unit and its matching header.  All mutable execution state
-- lives in the generated `NAME_vm` structure; generated code has no hidden
-- global mutable state.
--
-- Both spellings are supported:
--
--   local v = require("vmweave")
--   return v.vm { name = "Tiny", instructions = { { name = "HALT", flow = "halt" } } }
--
--   local vm = v.vm("Tiny")
--   vm:state("acc", { type = "i64" })
--   vm:instruction("ADD", { operands = { "cell" }, stack_effect = "( a b -- c )",
--                          semantics = "cell b=vm_pop(); cell a=vm_pop(); vm_push(a+b);" })
--   io.write(vm:emit({ dispatch = "switch" }))
--
-- Determinism: fields, instructions and components are emitted in sorted order;
-- Lua table iteration order is never observable in generated output.

local M = {}

-- VMWeave models a versioned STK-00 interchange.  This DSL emits C directly,
-- but it reports the same version so hosts can correlate artifacts.
M.version = "1"

-- ---------------------------------------------------------------------------
-- Symbol tables
-- ---------------------------------------------------------------------------

local state_c_types = {
  u8 = "uint8_t", u16 = "uint16_t", u32 = "uint32_t", u64 = "uint64_t",
  i8 = "int8_t", i16 = "int16_t", i32 = "int32_t", i64 = "int64_t",
  f32 = "float", f64 = "double",
  ptr = "void *", size = "size_t",
}

local cell_types = { intptr_t = true, int64_t = true, uint64_t = true }
local allocators = { custom = true, system = true, kalloc = true, jemalloc = true, memtkx = true, mimalloc = true }
local execution_models = { none = true, switch = true, subroutine = true, indirect = true, direct = true }
local flows = { next = true, jump = true, branch = true, call = true, ["return"] = true, halt = true }
local operand_kinds = { cell = true, label = true }

-- Component names in canonical emission order.  `jit` is recognised so a
-- description that asks for it receives a precise diagnostic instead of a
-- silent downgrade.
local component_order = {
  "token", "opcode", "insncode", "srtbl", "addrtbl", "insntbl", "dispatch",
  "ipc", "rewrite", "compile", "memory", "module", "jit", "tape", "frame",
  "optim", "atomic", "object",
}
local component_set = {}
for _, name in ipairs(component_order) do component_set[name] = true end
M.component_names = component_order

local default_components = { "token", "opcode", "insncode", "dispatch", "tape", "compile" }

local reserved_words = {}
for word in ([[
  auto break case char const continue default do double else enum extern float
  for goto if inline int long register restrict return short signed sizeof
  static struct switch typedef union unsigned void volatile while
  _Bool _Complex _Imaginary _Alignas _Alignof _Atomic _Generic _Noreturn
  _Static_assert _Thread_local
  alignas alignof bool class constexpr consteval constinit decltype delete
  explicit export false friend mutable namespace new noexcept nullptr operator
  private protected public requires static_assert template this thread_local
  throw true try typename using virtual
]]):gmatch("%S+") do reserved_words[word] = true end

-- Mnemonics that would collide with generated API surface for any VM.
local handler_reserved = {
  init = true, step = true, run = true, lookup = true, emit = true,
  compile = true, tape = true, rewrite = true, token = true, memory = true,
  object = true, atomic = true, ipc = true, module = true, optim = true,
  jit = true, thread = true, state = true, fail = true,
  call = true, ret = true, alloc = true, free = true,
}
local reserved_prefixes = {
  "vw_", "opcode_", "memory_", "object_", "atomic_", "ipc_", "module_",
  "optim_", "jit_", "tape_", "token_", "step_", "run_",
}

-- ---------------------------------------------------------------------------
-- Small helpers
-- ---------------------------------------------------------------------------

local function is_identifier(text)
  return type(text) == "string"
    and text:match("^[A-Za-z][A-Za-z0-9_]*$") ~= nil
    and not reserved_words[text]
end

local function is_integer(value, low, high)
  return type(value) == "number" and value == math.floor(value) and value >= low and value <= high
end

local function deep_copy(value)
  if type(value) ~= "table" then return value end
  local out = {}
  for key, item in pairs(value) do out[key] = deep_copy(item) end
  return out
end

local function check_keys(table_value, allowed, what)
  assert(type(table_value) == "table", "VMWeave: " .. what .. " must be a table")
  for key in pairs(table_value) do
    assert(allowed[key], "VMWeave: unknown " .. what .. " attribute '" .. tostring(key) .. "'")
  end
end

-- Provenance: report the first frame outside this module.  Chained builder
-- calls put the last call in tail position, so a fixed frame offset would
-- attribute the declaration to this file instead of the caller's.
local module_source = debug.getinfo(1, "S").source
local function current_location()
  for level = 2, 24 do
    local info = debug.getinfo(level, "Sl")
    if not info then break end
    if info.source ~= module_source then
      return { file = (info.source:gsub("^@", "")), line = math.max(info.currentline or 0, 0) }
    end
  end
  return { file = "<lua>", line = 0 }
end

-- C string literal escaping; generated sources must never contain an
-- unintended escape sequence.
local function c_string(text)
  local out = { '"' }
  for index = 1, #text do
    local byte = text:byte(index)
    local char = text:sub(index, index)
    if char == '"' or char == "\\" then
      out[#out + 1] = "\\" .. char
    elseif byte < 32 or byte >= 127 then
      out[#out + 1] = string.format("\\%03o", byte)
    else
      out[#out + 1] = char
    end
  end
  out[#out + 1] = '"'
  return table.concat(out)
end

local function qualify(text, name)
  return (text:gsub("@", name))
end

-- Stack-effect grammar: `( before -- after )` with whitespace-separated tokens.
local function effect_ok(text)
  if type(text) ~= "string" or text:find("\0", 1, true) then return false end
  local tokens = {}
  for token in text:gmatch("%S+") do tokens[#tokens + 1] = token end
  if tokens[1] ~= "(" then return false end
  local separator, closed = false, false
  for index = 2, #tokens do
    local token = tokens[index]
    if token == "--" then
      if separator or closed then return false end
      separator = true
    elseif token == ")" then
      if index ~= #tokens then return false end
      closed = true
      break
    elseif token:find("[%();\n\r]") then
      return false
    end
  end
  return separator and closed
end

local function effect_counts(effect)
  local before, after, separator = 0, 0, false
  for token in effect:gmatch("%S+") do
    if token == "--" then
      separator = true
    elseif token ~= "(" and token ~= ")" then
      if separator then after = after + 1 else before = before + 1 end
    end
  end
  return before, after
end

local function sorted_keys(map)
  local keys = {}
  for key in pairs(map) do keys[#keys + 1] = key end
  table.sort(keys)
  return keys
end

-- ---------------------------------------------------------------------------
-- Builder API
-- ---------------------------------------------------------------------------

local methods = {}
methods.__index = methods

local function receiver(self)
  assert(getmetatable(self) == methods, "VMWeave: use colon method calls on a VM description")
end

local function valid_state_name(name)
  return is_identifier(name) and name:sub(1, 5) ~= "vmweave_" and name:sub(1, 3) ~= "vw_"
end

local function valid_instruction_name(name)
  if not is_identifier(name) or handler_reserved[name] then return false end
  for _, prefix in ipairs(reserved_prefixes) do
    if name:sub(1, #prefix) == prefix then return false end
  end
  return true
end

function methods:state(name, attributes)
  receiver(self)
  assert(valid_state_name(name), "VMWeave: invalid or reserved state name '" .. tostring(name) .. "'")
  attributes = attributes or {}
  check_keys(attributes, { type = true, count = true }, "state")
  assert(state_c_types[attributes.type], "VMWeave: unknown state type '" .. tostring(attributes.type) .. "'")
  assert(attributes.count == nil or is_integer(attributes.count, 1, 1048576),
    "VMWeave: state count must be an integer in 1..1048576")
  for _, field in ipairs(self.fields) do
    assert(field.name ~= name, "VMWeave: duplicate state '" .. name .. "'")
  end
  self.fields[#self.fields + 1] = {
    name = name, type = attributes.type, count = attributes.count, source = current_location(),
  }
  return self
end

function methods:instruction(name, attributes)
  receiver(self)
  assert(valid_instruction_name(name), "VMWeave: invalid or reserved instruction name '" .. tostring(name) .. "'")
  attributes = attributes or {}
  check_keys(attributes,
    { opcode = true, body = true, semantics = true, operands = true, stack_effect = true,
      flow = true, source = true },
    "instruction")
  assert(not (attributes.body and attributes.semantics), "VMWeave: body and semantics are aliases")
  local semantics = attributes.semantics or attributes.body or ""
  assert(type(semantics) == "string" and not semantics:find("\0", 1, true),
    "VMWeave: instruction semantics must be C text without NUL bytes")
  assert(attributes.opcode == nil or is_integer(attributes.opcode, 0, 2147483647),
    "VMWeave: opcode must be an integer in 0..2147483647")
  local effect = attributes.stack_effect or "( -- )"
  assert(effect_ok(effect), "VMWeave: stack effect must read '( before -- after )'")
  local flow = attributes.flow or "next"
  assert(flows[flow], "VMWeave: unknown control flow '" .. tostring(flow) .. "'")
  local operands = deep_copy(attributes.operands or {})
  assert(#operands <= 16, "VMWeave: at most 16 operands are supported")
  for _, operand in ipairs(operands) do
    assert(operand_kinds[operand], "VMWeave: unsupported operand kind '" .. tostring(operand) .. "'")
  end
  for _, existing in ipairs(self.instructions) do
    assert(existing.name ~= name, "VMWeave: duplicate instruction '" .. name .. "'")
    assert(attributes.opcode == nil or attributes.opcode ~= existing.opcode, "VMWeave: duplicate opcode")
  end
  local source = attributes.source
  if source then
    check_keys(source, { file = true, line = true }, "source")
    assert(type(source.file) == "string" and is_integer(source.line, 0, 2147483647),
      "VMWeave: source needs { file = string, line = integer }")
  else
    source = current_location()
  end
  self.instructions[#self.instructions + 1] = {
    name = name, semantics = semantics, effect = effect, flow = flow,
    operands = operands, opcode = attributes.opcode, source = source,
  }
  return self
end

function methods:set_components(names)
  receiver(self)
  assert(type(names) == "table", "VMWeave: components must be a sequence")
  self.components = deep_copy(names)
  return self
end

function methods:rewrite(rule)
  receiver(self)
  check_keys(rule, { from = true, to = true }, "rewrite")
  assert(type(rule.from) == "table" and type(rule.to) == "table",
    "VMWeave: rewrite needs from/to sequences")
  self.rewrites[#self.rewrites + 1] = { from = deep_copy(rule.from), to = deep_copy(rule.to) }
  return self
end

-- ---------------------------------------------------------------------------
-- Construction
-- ---------------------------------------------------------------------------

local spec_keys = {
  name = true, execution = true, stack = true, fields = true, instructions = true,
  components = true, hooks = true, memory = true, subsystems = true, rewrites = true,
  frame_capacity = true, ipc_capacity = true, source = true,
}

local instruction_keys = {
  name = true, opcode = true, body = true, semantics = true, operands = true,
  stack_effect = true, flow = true, source = true,
}

-- Forward declarations: the builder validates a description before returning
-- it, while evaluation and lowering are defined below.
local check, spec_of

function M.vm(description)
  local spec
  if type(description) == "string" then
    -- Legacy shorthand: plain handlers, no dispatcher.
    spec = { name = description, execution = "none", components = { "insncode" } }
  else
    check_keys(description, spec_keys, "VM description")
    spec = deep_copy(description)
  end
  assert(is_identifier(spec.name), "VMWeave: invalid VM name '" .. tostring(spec.name) .. "'")
  if spec.source then
    check_keys(spec.source, { file = true, line = true }, "source")
  else
    spec.source = current_location()
  end
  if spec.stack then check_keys(spec.stack, { cell = true, capacity = true }, "stack") end
  if spec.memory then check_keys(spec.memory, { allocator = true }, "memory") end

  local vm = setmetatable({}, methods)
  vm.name = spec.name
  vm.execution = spec.execution or "switch"
  vm.cell = (spec.stack and spec.stack.cell) or "intptr_t"
  vm.stack_capacity = (spec.stack and spec.stack.capacity) or 1024
  vm.frame_capacity = spec.frame_capacity or 64
  vm.ipc_capacity = spec.ipc_capacity or 64
  vm.allocator = (spec.memory and spec.memory.allocator) or "custom"
  vm.hooks = spec.hooks or false
  vm.components = spec.components and deep_copy(spec.components) or deep_copy(default_components)
  vm.subsystems = spec.subsystems and deep_copy(spec.subsystems) or {}
  vm.rewrites = spec.rewrites and deep_copy(spec.rewrites) or {}
  vm.source = spec.source
  vm.fields = {}
  vm.instructions = {}

  for _, field in ipairs(spec.fields or {}) do
    check_keys(field, { name = true, type = true, count = true }, "field")
    vm:state(field.name, { type = field.type, count = field.count })
  end
  for _, word in ipairs(spec.instructions or {}) do
    check_keys(word, instruction_keys, "instruction")
    local attributes = deep_copy(word)
    local name = attributes.name
    attributes.name = nil
    vm:instruction(name, attributes)
  end
  -- Structural errors (unknown components, unsatisfied component dependencies,
  -- reserved names) fail at construction rather than at first emission.
  check(spec_of(vm, vm.components, vm.execution, vm.hooks))
  M.last = vm
  return vm
end

-- Load a `.vmw` specification file and return the resulting VM description.
function M.load(path)
  local chunk, message = loadfile(path)
  assert(chunk, message)
  local result = chunk()
  assert(getmetatable(result) == methods, "VMWeave: " .. path .. " did not return a VM description")
  return result
end

-- ---------------------------------------------------------------------------
-- Validation and lowering
-- ---------------------------------------------------------------------------

-- Assemble a plain specification table from a builder description.  Kept
-- separate so construction and emission share one validation path.
spec_of = function(vm, components, execution, hooks)
  return {
    name = vm.name, execution = execution,
    cell = vm.cell, stack_capacity = vm.stack_capacity,
    frame_capacity = vm.frame_capacity, ipc_capacity = vm.ipc_capacity,
    allocator = vm.allocator, hooks = hooks,
    components = deep_copy(components), subsystems = deep_copy(vm.subsystems),
    rewrites = deep_copy(vm.rewrites), fields = deep_copy(vm.fields),
    instructions = deep_copy(vm.instructions), source = vm.source,
  }
end

check = function(spec)
  local where = string.format("%s:%d:", spec.source.file, spec.source.line)
  local function fail(message)
    error("VMWeave: " .. where .. " VM '" .. spec.name .. "': " .. message, 0)
  end
  if not execution_models[spec.execution] then
    fail("unknown execution model '" .. tostring(spec.execution) .. "'")
  end
  if not cell_types[spec.cell] then fail("cell must be intptr_t, int64_t, or uint64_t") end
  for _, capacity in ipairs({ spec.stack_capacity, spec.frame_capacity, spec.ipc_capacity }) do
    if not is_integer(capacity, 1, 1048576) then fail("capacities must be integers in 1..1048576") end
  end
  if type(spec.hooks) ~= "boolean" then fail("hooks must be Boolean") end
  if spec.hooks and spec.execution == "none" then fail("hooks require a dispatch model") end
  if not allocators[spec.allocator] then fail("unknown allocator '" .. tostring(spec.allocator) .. "'") end

  local components = {}
  for _, component in ipairs(spec.components) do
    if not component_set[component] then fail("unknown component '" .. tostring(component) .. "'") end
    if components[component] then fail("duplicate component '" .. component .. "'") end
    components[component] = true
  end
  if components.jit then
    fail("component 'jit' needs the native backend, which this generator does not provide")
  end
  for _, component in ipairs(sorted_keys(spec.subsystems)) do
    if not components[component] then fail("C subsystem '" .. component .. "' is not selected") end
    local body = spec.subsystems[component]
    if type(body) ~= "string" or body:find("\0", 1, true) then
      fail("subsystem '" .. component .. "' needs C text without NUL bytes")
    end
  end
  if components.object and not components.memory then fail("object requires memory") end
  if components.compile and not components.tape then fail("compile requires tape") end
  for _, pair in ipairs({ { "srtbl", "subroutine" }, { "addrtbl", "indirect" }, { "insntbl", "direct" } }) do
    if components[pair[1]] and spec.execution ~= pair[2] then
      fail(pair[1] .. " requires " .. pair[2] .. " execution")
    end
  end
  if components.dispatch and spec.execution == "none" then fail("dispatch requires an execution model") end
  if components.dispatch and spec.execution == "subroutine" and not components.srtbl then
    fail("subroutine dispatch requires srtbl")
  end
  if components.dispatch and spec.execution == "indirect" and not components.addrtbl then
    fail("indirect dispatch requires addrtbl")
  end
  if components.dispatch and spec.execution == "direct" and not components.insntbl then
    fail("direct dispatch requires insntbl")
  end

  local fields = {}
  for _, field in ipairs(spec.fields) do
    if not valid_state_name(field.name) then
      fail("invalid or reserved state name '" .. tostring(field.name) .. "'")
    end
    if fields[field.name] then fail("duplicate state '" .. field.name .. "'") end
    fields[field.name] = true
    if not state_c_types[field.type] then fail("unknown state type '" .. tostring(field.type) .. "'") end
    if field.count ~= nil and not is_integer(field.count, 1, 1048576) then
      fail("state '" .. field.name .. "' has an invalid array count")
    end
  end

  local seen = {}
  for _, word in ipairs(spec.instructions) do
    local prefix = "instruction '" .. tostring(word.name) .. "': "
    if not valid_instruction_name(word.name) then fail(prefix .. "invalid or reserved name") end
    if seen[word.name] then fail(prefix .. "duplicate mnemonic") end
    seen[word.name] = true
    if word.opcode ~= nil and not is_integer(word.opcode, 0, 2147483647) then fail(prefix .. "invalid opcode") end
    if not effect_ok(word.effect) then fail(prefix .. "expected stack effect '( before -- after )'") end
    if not flows[word.flow] then fail(prefix .. "unknown control flow") end
    if (word.flow == "call" or word.flow == "return") and not components.frame then
      fail(prefix .. "call/return requires the frame component")
    end
    if #word.operands > 16 then fail(prefix .. "at most 16 operands are supported") end
    for _, operand in ipairs(word.operands) do
      if not operand_kinds[operand] then
        fail(prefix .. "unsupported operand kind '" .. tostring(operand) .. "'")
      end
    end
    if type(word.semantics) ~= "string" or word.semantics:find("\0", 1, true) then
      fail(prefix .. "semantics must be C text without NUL bytes")
    end
  end

  local words = {}
  for _, word in ipairs(spec.instructions) do words[word.name] = word end
  for _, rule in ipairs(spec.rewrites) do
    if #rule.from == 0 or #rule.from > 32 or #rule.to >= #rule.from then
      fail("rewrite must strictly shorten a nonempty sequence")
    end
    local profiles = {}
    for _, sequence in ipairs({ rule.from, rule.to }) do
      local depth, required, peak = 0, 0, 0
      for _, name in ipairs(sequence) do
        local word = words[name]
        if not word or #word.operands ~= 0 or word.flow ~= "next" then
          fail("rewrite requires known operand-free straight-line instructions")
        end
        local inputs, outputs = effect_counts(word.effect)
        if depth < inputs then required = math.max(required, inputs - depth) end
        depth = depth + outputs - inputs
        peak = math.max(peak, depth)
      end
      profiles[#profiles + 1] = { depth = depth, required = required, peak = peak }
    end
    local from, to = profiles[1], profiles[2]
    if from.depth ~= to.depth or to.required > from.required or to.peak > from.peak then
      fail("rewrite has incompatible stack effects")
    end
  end
  if #spec.rewrites > 0 and not components.rewrite then fail("rewrite rules require the rewrite component") end

  return components
end

-- Assign opcodes deterministically: explicit ones are honoured, missing ones
-- take the lowest unused nonnegative integer in sorted mnemonic order.
local function lower(spec, components)
  local words = {}
  for _, word in ipairs(spec.instructions) do words[#words + 1] = word end
  table.sort(words, function(left, right) return left.name < right.name end)
  local fields = {}
  for _, field in ipairs(spec.fields) do fields[#fields + 1] = field end
  table.sort(fields, function(left, right) return left.name < right.name end)
  local components_list = {}
  for _, component in ipairs(component_order) do
    if components[component] then components_list[#components_list + 1] = component end
  end
  local used = {}
  for _, word in ipairs(words) do
    if word.opcode ~= nil then used[word.opcode] = true end
  end
  local next_opcode = 0
  for _, word in ipairs(words) do
    if word.opcode == nil then
      while used[next_opcode] do next_opcode = next_opcode + 1 end
      word.opcode = next_opcode
      used[next_opcode] = true
    end
  end
  return {
    name = spec.name, execution = spec.execution, cell = spec.cell,
    stack_capacity = spec.stack_capacity, frame_capacity = spec.frame_capacity,
    ipc_capacity = spec.ipc_capacity, allocator = spec.allocator,
    hooks = spec.hooks, source = spec.source, rewrites = spec.rewrites,
    subsystems = spec.subsystems, fields = fields, words = words,
    components = components_list, selected = components,
  }
end

-- Structural ABI fingerprint: covers the cell type, field layout contract,
-- capacities and selected components so a host can reject a mismatch.
local function abi_fingerprint(module)
  local parts = { module.name, module.cell, tostring(module.stack_capacity),
    tostring(module.frame_capacity), tostring(module.ipc_capacity) }
  for _, field in ipairs(module.fields) do
    parts[#parts + 1] = field.name .. ":" .. field.type .. ":" .. tostring(field.count or 0)
  end
  parts[#parts + 1] = table.concat(module.components, ",")
  local text = table.concat(parts, "|")
  -- FNV-1a.  Lua 5.3+ has exact 64-bit integer wraparound, so the reference
  -- hash is computed directly; hosts with only 5.1 semantics fall back to a
  -- 32-bit rolling hash that is still deterministic.
  local native_integers = math.maxinteger ~= nil and 1 // 1 == 1
  local hash
  if native_integers then
    hash = 0xcbf29ce484222325
    for index = 1, #text do
      hash = (hash ~ text:byte(index)) * 0x100000001b3
    end
  else
    hash = 2166136261
    for index = 1, #text do
      hash = (hash * 16777619 + text:byte(index)) % 4294967296
    end
  end
  return string.format("0x%x", hash)
end

-- ---------------------------------------------------------------------------
-- C emission
-- ---------------------------------------------------------------------------

local function selected(module, component)
  return module.selected[component] == true
end

local core_runtime = [[
static inline void @_vw_fail(@_vm *vm, int status) { if(vm && !vm->vw_error) vm->vw_error = status ? status : -1; }
static inline @_cell @_vw_pop(@_vm *vm) {
  if(!vm || vm->vw_error) return 0;
  if(!vm->vw_sp) { @_vw_fail(vm,-2); return 0; }
  if(vm->vw_sp > sizeof(vm->vw_stack)/sizeof(vm->vw_stack[0])) { @_vw_fail(vm,-3); return 0; }
  return vm->vw_stack[--vm->vw_sp];
}
static inline void @_vw_push(@_vm *vm, @_cell value) {
  if(!vm || vm->vw_error) return;
  if(vm->vw_sp >= sizeof(vm->vw_stack)/sizeof(vm->vw_stack[0])) { @_vw_fail(vm,-3); return; }
  vm->vw_stack[vm->vw_sp++] = value;
}
static inline @_cell @_vw_operand(@_vm *vm, size_t index) {
  if(!vm || !vm->vw_current || index >= vm->vw_current->operand_count) { @_vw_fail(vm,-4); return 0; }
  return vm->vw_current->operands[index];
}
static inline void @_vw_jump(@_vm *vm, @_cell target) {
  if(!vm || vm->vw_error) return;
  if((uintmax_t)target >= vm->vw_code_size) { @_vw_fail(vm,-4); return; }
  vm->vw_pc = (size_t)target;
}
static inline void @_vw_halt(@_vm *vm) { if(vm) vm->vw_halted=1; }
]]

local check_runtime = [[
static inline int @_vw_check(@_vm *vm, uint32_t opcode, size_t *base, const @_metadata **metadata) {
  const @_metadata *info = @_lookup(opcode);
  if(!vm || !info) return -1;
  if(vm->vw_error) return vm->vw_error;
  if(info->operands && (!vm->vw_current || vm->vw_current->operand_count != info->operands)) return -4;
  if(vm->vw_sp < info->inputs) { @_vw_fail(vm,-2); return -2; }
  *base = vm->vw_sp - info->inputs;
  if(*base > sizeof(vm->vw_stack)/sizeof(vm->vw_stack[0]) || info->outputs > sizeof(vm->vw_stack)/sizeof(vm->vw_stack[0]) - *base) { @_vw_fail(vm,-3); return -3; }
  *metadata = info; return 0;
}
static inline int @_vw_finish(@_vm *vm, size_t base, const @_metadata *info) {
  if(!vm->vw_error && vm->vw_sp != base + info->outputs) @_vw_fail(vm,-6);
  return vm->vw_error;
}
]]

local validate_runtime = [[
static inline int @_vw_label_operand(uint32_t opcode, size_t index) {
  (void)index; switch(opcode) {
@cases
  default: return 0;
  }
}
static inline int @_vw_validate_instruction(const @_instruction *instruction, size_t size) {
  size_t j; const @_metadata *info;
  if(!instruction) return -1;
  info=@_lookup(instruction->opcode);
  if(!info || info->operands!=instruction->operand_count) return -4;
  for(j=0;j<info->operands;++j)
    if(@_vw_label_operand(info->opcode,j) && (uintmax_t)instruction->operands[j]>=size) return -4;
  return 0;
}
]]

local function label_cases(module)
  local out = {}
  for _, word in ipairs(module.words) do
    local conditions = {}
    for index, operand in ipairs(word.operands) do
      if operand == "label" then conditions[#conditions + 1] = "index==" .. (index - 1) end
    end
    local condition = #conditions > 0 and (" || " .. table.concat(conditions, " || ")) or ""
    out[#out + 1] = "  case " .. word.opcode .. "u: return 0" .. condition .. ";"
  end
  return table.concat(out, "\n")
end

local function metadata_table(module)
  local name = module.name
  local out = { "static const " .. name .. "_metadata " .. name .. "_vw_table[] = {" }
  for _, word in ipairs(module.words) do
    local inputs, outputs = effect_counts(word.effect)
    out[#out + 1] = string.format("  {%s, %du, %d, %d, %d, %s},",
      c_string(word.name), word.opcode, #word.operands, inputs, outputs, c_string(word.flow))
  end
  out[#out + 1] = "  {NULL, 0u, 0, 0, 0, NULL}"
  out[#out + 1] = "};"
  out[#out + 1] = "static inline const " .. name .. "_metadata *" .. name .. "_lookup(uint32_t opcode) {"
  out[#out + 1] = "  size_t i; for(i=0;" .. name .. "_vw_table[i].name;++i) if(" .. name
    .. "_vw_table[i].opcode==opcode) return &" .. name .. "_vw_table[i];"
  out[#out + 1] = "  return NULL;"
  out[#out + 1] = "}"
  return table.concat(out, "\n")
end

local function optional_types(module)
  local name = module.name
  local out = {}
  if selected(module, "memory") then
    out[#out + 1] = qualify([[
typedef struct {
  void *context;
  void *(*allocate)(void *, size_t);
  void *(*reallocate)(void *, void *, size_t);
  void (*release)(void *, void *);
} @_allocator;
]], name)
  end
  if selected(module, "frame") then
    out[#out + 1] = qualify(
      "typedef struct { size_t return_address, stack_base; @_cell locals[16]; } @_activation;", name)
  end
  if selected(module, "object") then
    out[#out + 1] = qualify(
      "typedef struct @_object { struct @_object *next; uint32_t kind; size_t size; unsigned char data[]; } @_object;",
      name)
  end
  if selected(module, "module") then
    out[#out + 1] = qualify([[
typedef struct { const char *name; void *address; } @_symbol;
typedef struct { const char *name; const @_symbol *exports; size_t export_count; const @_tape *code; } @_module;
]], name)
  end
  if selected(module, "atomic") then
    out[#out + 1] = qualify([[
enum { @_nil=0, @_integer=1, @_floating=2, @_boolean=3, @_reference=4 };
typedef struct { uint32_t tag; union { @_cell integer; double floating; int boolean; void *reference; } value; } @_atomic;
]], name)
  end
  if selected(module, "optim") then
    out[#out + 1] = qualify(
      "typedef struct { uintptr_t key; void *value; uint64_t epoch; } @_cache_entry;", name)
  end
  return table.concat(out, "\n")
end

local function struct_members(module)
  local name = module.name
  local out = { "struct " .. name .. "_vm {" }
  for _, field in ipairs(module.fields) do
    local text = "  " .. state_c_types[field.type] .. " " .. field.name
    if field.count then text = text .. "[" .. field.count .. "]" end
    out[#out + 1] = text .. ";"
  end
  out[#out + 1] = "  " .. name .. "_cell vw_stack[" .. module.stack_capacity .. "];"
  out[#out + 1] = "  size_t vw_sp, vw_pc, vw_code_size;"
  out[#out + 1] = "  int vw_error, vw_halted;"
  out[#out + 1] = "  const " .. name .. "_instruction *vw_current;"
  if module.hooks then
    out[#out + 1] = "  void *vmweave_userdata;"
    out[#out + 1] = "  int (*vmweave_before)(" .. name .. "_vm *, uint32_t, void *);"
    out[#out + 1] = "  void (*vmweave_after)(" .. name .. "_vm *, uint32_t, void *);"
  end
  if selected(module, "memory") then out[#out + 1] = "  " .. name .. "_allocator vw_allocator;" end
  if selected(module, "object") then out[#out + 1] = "  " .. name .. "_object *vw_objects;" end
  if selected(module, "module") then out[#out + 1] = "  const " .. name .. "_module *vw_module;" end
  if selected(module, "frame") then
    out[#out + 1] = "  " .. name .. "_activation vw_frames[" .. module.frame_capacity .. "];"
    out[#out + 1] = "  size_t vw_frame_count;"
  end
  if selected(module, "ipc") then
    out[#out + 1] = "  " .. name .. "_cell vw_messages[" .. module.ipc_capacity .. "];"
    out[#out + 1] = "  size_t vw_message_head, vw_message_count;"
  end
  if selected(module, "optim") then
    out[#out + 1] = "  " .. name .. "_cache_entry vw_cache[16];"
    out[#out + 1] = "  uint64_t vw_epoch;"
  end
  out[#out + 1] = "};"
  return table.concat(out, "\n")
end

local function prototypes(module)
  local name = module.name
  local out = {}
  for _, word in ipairs(module.words) do
    out[#out + 1] = "void " .. name .. "_" .. word.name .. "(" .. name .. "_vm *vm);"
  end
  if selected(module, "opcode") then
    for _, word in ipairs(module.words) do
      out[#out + 1] = "#define " .. name .. "_opcode_" .. word.name .. " " .. word.opcode .. "u"
    end
  end
  if selected(module, "dispatch") then
    out[#out + 1] = "int " .. name .. "_step(" .. name .. "_vm *, uint32_t);"
    if module.execution == "direct" then
      out[#out + 1] = "int " .. name .. "_run_direct(" .. name .. "_vm *, const " .. name
        .. "_threaded *, size_t, size_t);"
      out[#out + 1] = "int " .. name .. "_thread(const " .. name .. "_tape *, " .. name
        .. "_threaded *, size_t);"
    else
      out[#out + 1] = "int " .. name .. "_run(" .. name .. "_vm *, const " .. name .. "_tape *, size_t);"
    end
  end
  local decls = {
    token = "const char *@_token(uint32_t);",
    compile = "int @_compile(const char *, @_tape *);",
    tape = "int @_tape_validate(const @_tape *);",
    rewrite = "int @_rewrite(@_tape *);",
    memory = "int @_memory_set(@_vm *, @_allocator);\nvoid *@_alloc(@_vm *, size_t);\nvoid *@_memory_resize(@_vm *, void *, size_t);\nvoid @_free(@_vm *, void *);",
    frame = "int @_call(@_vm *, @_cell);\nint @_ret(@_vm *);",
    object = "@_object *@_object_new(@_vm *, uint32_t, size_t);\nint @_object_read(const @_object *, size_t, void *, size_t);\nint @_object_write(@_object *, size_t, const void *, size_t);\nint @_object_delete(@_vm *, @_object *);\nint @_object_clear(@_vm *);",
    ipc = "int @_ipc_send(@_vm *, @_cell);\nint @_ipc_receive(@_vm *, @_cell *);",
    module = "void *@_module_find(const @_module *, const char *);\nint @_module_validate(const @_module *);\nint @_module_load(@_vm *, const @_module *);",
    optim = "void @_optim_invalidate(@_vm *);\nvoid @_optim_store(@_vm *, uintptr_t, void *);\nvoid *@_optim_lookup(@_vm *, uintptr_t);",
    atomic = "@_atomic @_atomic_integer(@_cell);\n@_atomic @_atomic_float(double);\n@_atomic @_atomic_bool(int);\n@_atomic @_atomic_reference(void *);\n@_atomic @_atomic_nil(void);",
    srtbl = "@_handler @_vw_routine(uint32_t);",
    addrtbl = "const @_handler *@_vw_address(uint32_t);",
    insntbl = "@_handler @_vw_instruction(uint32_t);",
  }
  for _, component in ipairs(component_order) do
    if selected(module, component) and decls[component] then
      out[#out + 1] = qualify(decls[component], name)
    end
  end
  if selected(module, "memory") and module.allocator == "system" then
    out[#out + 1] = "int " .. name .. "_memory_system(" .. name .. "_vm *);"
  end
  return table.concat(out, "\n")
end

local function gen_header(module)
  local name = module.name
  local out = { "/* Generated by VMWeave from a declarative description. Do not edit. */" }
  out[#out + 1] = "#ifndef " .. name .. "_VMWEAVE_H"
  out[#out + 1] = "#define " .. name .. "_VMWEAVE_H"
  out[#out + 1] = "#include <stdint.h>"
  out[#out + 1] = "#include <stddef.h>"
  out[#out + 1] = "#include <stdlib.h>"
  out[#out + 1] = "#include <string.h>"
  out[#out + 1] = "#include <limits.h>"
  out[#out + 1] = "#ifdef __cplusplus\nextern \"C\" {\n#endif"
  out[#out + 1] = "typedef " .. module.cell .. " " .. name .. "_cell;"
  out[#out + 1] = "typedef struct " .. name .. "_vm " .. name .. "_vm;"
  out[#out + 1] = "typedef void (*" .. name .. "_handler)(" .. name .. "_vm *);"
  out[#out + 1] = qualify([[
typedef struct { uint32_t opcode; size_t operand_count; @_cell operands[16]; } @_instruction;
typedef struct { @_instruction *data; size_t size, capacity; } @_tape;
typedef struct { const char *name; uint32_t opcode; size_t operands, inputs, outputs; const char *flow; } @_metadata;
]], name)
  if module.execution == "direct" then
    out[#out + 1] = qualify(
      "typedef struct { @_handler address; @_instruction instruction; } @_threaded;", name)
  end
  local extras = optional_types(module)
  if extras ~= "" then out[#out + 1] = extras end
  out[#out + 1] = struct_members(module)
  out[#out + 1] = "static inline uint64_t " .. name .. "_vw_abi(void) { return UINT64_C("
    .. abi_fingerprint(module) .. "); }"
  -- The metadata table and lookup are needed by the inline runtime below, so
  -- they must precede it.  `insncode`/`token`/`dispatch` all resolve through it.
  out[#out + 1] = metadata_table(module)
  out[#out + 1] = qualify(core_runtime, name)
  out[#out + 1] = qualify((validate_runtime:gsub("@cases", (label_cases(module):gsub("%%", "%%%%")))), name)
  out[#out + 1] = qualify(check_runtime, name)
  out[#out + 1] = "static inline void " .. name .. "_init(" .. name
    .. "_vm *vm) { if(vm) memset(vm, 0, sizeof(*vm)); }"
  out[#out + 1] = prototypes(module)
  out[#out + 1] = "#ifdef __cplusplus\n}\n#endif"
  out[#out + 1] = "#endif"
  return table.concat(out, "\n") .. "\n"
end

local handler_macros = [[
#define vm_pop() @_vw_pop(vm)
#define vm_push(x) @_vw_push(vm,(x))
#define vm_operand(i) @_vw_operand(vm,(i))
#define vm_jump(x) @_vw_jump(vm,(x))
#define vm_halt() @_vw_halt(vm)
#define vm_fail(x) @_vw_fail(vm,(x))
]]

local function gen_handlers(module)
  local name = module.name
  local macros = handler_macros
  if selected(module, "frame") then
    macros = macros .. "#define vm_call(x) @_call(vm,(x))\n#define vm_return() @_ret(vm)\n"
  end
  if selected(module, "memory") then
    macros = macros .. "#define vm_alloc(x) @_alloc(vm,(x))\n#define vm_free(x) @_free(vm,(x))\n"
  end
  local out = { qualify(macros, name) }
  for _, word in ipairs(module.words) do
    out[#out + 1] = "void " .. name .. "_" .. word.name .. "(" .. name .. "_vm *vm) {"
    out[#out + 1] = "  typedef " .. name .. "_cell cell; (void)sizeof(cell); (void)vm;"
    for index = 0, #word.operands - 1 do
      out[#out + 1] = "  cell operand" .. index .. " = vm_operand(" .. index .. "); (void)operand" .. index .. ";"
    end
    if word.source.file and word.source.file ~= "" and word.source.line and word.source.line > 0 then
      out[#out + 1] = "#line " .. word.source.line .. " " .. c_string(word.source.file)
    end
    out[#out + 1] = word.semantics
    out[#out + 1] = "#line 1 \"<VMWeave generated>\""
    out[#out + 1] = "}"
  end
  for _, macro in ipairs({ "vm_pop", "vm_push", "vm_operand", "vm_jump", "vm_halt", "vm_fail",
    "vm_call", "vm_return", "vm_alloc", "vm_free" }) do
    out[#out + 1] = "#undef " .. macro
  end
  return table.concat(out, "\n")
end

local function gen_table(module)
  local name = module.name
  local component = module.execution == "subroutine" and "srtbl"
    or (module.execution == "indirect" and "addrtbl" or "insntbl")
  local indirect = component == "addrtbl"
  local method = component == "srtbl" and "routine" or (indirect and "address" or "instruction")
  local out = {}
  out[#out + 1] = (indirect and "const " or "") .. name .. "_handler " .. (indirect and "*" or "")
    .. name .. "_vw_" .. method .. "(uint32_t opcode) {"
  if indirect then
    for _, word in ipairs(module.words) do
      out[#out + 1] = "  static const " .. name .. "_handler slot_" .. word.name .. " = " .. name
        .. "_" .. word.name .. ";"
    end
  end
  out[#out + 1] = "  switch(opcode) {"
  for _, word in ipairs(module.words) do
    out[#out + 1] = "  case " .. word.opcode .. "u: return "
      .. (indirect and "&slot_" or (name .. "_")) .. word.name .. ";"
  end
  out[#out + 1] = "  default: return NULL;"
  out[#out + 1] = "  }"
  out[#out + 1] = "}"
  return table.concat(out, "\n")
end

local function gen_dispatch(module)
  local name = module.name
  local out = {}
  out[#out + 1] = "int " .. name .. "_step(" .. name .. "_vm *vm, uint32_t opcode) {"
  out[#out + 1] = "  size_t base=0; const " .. name .. "_metadata *info=NULL;"
  out[#out + 1] = "  int status=" .. name .. "_vw_check(vm,opcode,&base,&info); if(status) return status;"
  if module.hooks then
    out[#out + 1] = "  if(vm->vmweave_before) { status=vm->vmweave_before(vm,opcode,vm->vmweave_userdata);"
      .. " if(status) return status; }"
  end
  if module.execution == "switch" or module.execution == "direct" then
    out[#out + 1] = "  switch(opcode) {"
    for _, word in ipairs(module.words) do
      out[#out + 1] = "  case " .. word.opcode .. "u: " .. name .. "_" .. word.name .. "(vm); break;"
    end
    out[#out + 1] = "  default: return -1;"
    out[#out + 1] = "  }"
  elseif module.execution == "subroutine" then
    out[#out + 1] = "  " .. name .. "_handler fn=" .. name .. "_vw_routine(opcode); if(!fn) return -1; fn(vm);"
  else
    out[#out + 1] = "  const " .. name .. "_handler *slot=" .. name
      .. "_vw_address(opcode); if(!slot || !*slot) return -1; (*slot)(vm);"
  end
  if module.hooks then
    out[#out + 1] = "  if(vm->vmweave_after) vm->vmweave_after(vm,opcode,vm->vmweave_userdata);"
  end
  out[#out + 1] = "  return " .. name .. "_vw_finish(vm,base,info);"
  out[#out + 1] = "}"
  if module.execution == "direct" then
    out[#out + 1] = qualify([[
int @_thread(const @_tape *tape, @_threaded *out, size_t capacity) {
  size_t i;
  if(!tape || tape->size > tape->capacity || tape->size > capacity || (tape->size && (!out || !tape->data))) return -1;
  for(i=0;i<tape->size;++i) if(@_vw_validate_instruction(&tape->data[i],tape->size)) return -4;
  for(i=0;i<tape->size;++i) { out[i].address=@_vw_instruction(tape->data[i].opcode); out[i].instruction=tape->data[i]; }
  return 0;
}
int @_run_direct(@_vm *vm, const @_threaded *code, size_t size, size_t budget) {
  size_t steps=0, i; int status=0;
  if(!vm || (size && !code)) return -1;
  if(vm->vw_pc>size) return -4;
  for(i=0;i<size;++i) if(@_vw_validate_instruction(&code[i].instruction,size) || !code[i].address || code[i].address!=@_vw_instruction(code[i].instruction.opcode)) return -4;
  vm->vw_code_size=size; vm->vw_halted=0;
  while(!vm->vw_halted && vm->vw_pc<size) {
    const @_threaded *entry; const @_metadata *info=NULL; size_t base=0, origin=vm->vw_pc;
    if(steps++>=budget) { status=-7; break; }
    entry=&code[vm->vw_pc];
    if(!entry->address || entry->address!=@_vw_instruction(entry->instruction.opcode)) {status=-4; break;}
    vm->vw_current=&entry->instruction;
    status=@_vw_check(vm,entry->instruction.opcode,&base,&info); if(status) break;
]], name)
    if module.hooks then
      out[#out + 1] = "    if(vm->vmweave_before) {status=vm->vmweave_before(vm,entry->instruction.opcode,"
        .. "vm->vmweave_userdata); if(status) break;}"
    end
    out[#out + 1] = "    ++vm->vw_pc; entry->address(vm);"
    if module.hooks then
      out[#out + 1] = "    if(vm->vmweave_after) vm->vmweave_after(vm,entry->instruction.opcode,"
        .. "vm->vmweave_userdata);"
    end
    out[#out + 1] = "    status=" .. name .. "_vw_finish(vm,base,info); if(status) {vm->vw_pc=origin; break;}"
    out[#out + 1] = "  }"
    out[#out + 1] = "  vm->vw_current=NULL; return status;"
    out[#out + 1] = "}"
  else
    out[#out + 1] = qualify([[
int @_run(@_vm *vm, const @_tape *tape, size_t budget) {
  size_t steps=0, i; int status=0;
  if(!vm || !tape || tape->size>tape->capacity || (tape->size && !tape->data)) return -1;
  if(vm->vw_pc>tape->size) return -4;
  for(i=0;i<tape->size;++i) if(@_vw_validate_instruction(&tape->data[i],tape->size)) return -4;
  vm->vw_code_size=tape->size; vm->vw_halted=0;
  while(!vm->vw_halted && vm->vw_pc<tape->size) {
    const @_metadata *info; size_t origin=vm->vw_pc;
    if(steps++>=budget) {status=-7; break;}
    vm->vw_current=&tape->data[vm->vw_pc]; info=@_lookup(vm->vw_current->opcode);
    if(!info || vm->vw_current->operand_count!=info->operands) {status=-4; break;}
    ++vm->vw_pc; status=@_step(vm,vm->vw_current->opcode); if(status) {vm->vw_pc=origin; break;}
  }
  vm->vw_current=NULL; return status;
}
]], name)
  end
  return table.concat(out, "\n")
end

local function gen_compiler(module)
  local name = module.name
  local mnemonics = {}
  for _, word in ipairs(module.words) do
    mnemonics[#mnemonics + 1] = "  if(strcmp(name," .. c_string(word.name) .. ")==0) return "
      .. name .. "_lookup(" .. word.opcode .. "u);"
  end
  if #mnemonics == 0 then mnemonics[1] = "  (void)name;" end
  -- Marker expansion happens after the `@name` qualification pass, so the
  -- markers themselves deliberately avoid `@`.
  local text = [[
#include <inttypes.h>
#include <errno.h>
#include <ctype.h>
typedef struct { char name[128]; size_t address; } @_vw_label;
static int @_vw_token(const char **source, char *out, size_t capacity) {
  const char *p=*source; size_t count=0;
  for(;;) {
    while(*p && isspace((unsigned char)*p)) ++p;
    if(*p!='#') break;
    while(*p && *p!='\n') ++p;
  }
  if(!*p) { *source=p; return 0; }
  while(*p && !isspace((unsigned char)*p) && *p!='#') {
    if(count+1>=capacity) return -1;
    out[count++]=*p++;
  }
  out[count]=0; *source=p; return 1;
}
static const @_metadata *@_vw_mnemonic(const char *name) {
__MNEMONICS__
  return NULL;
}
/* Caller-owned output, transactional validation, labels are instruction indices. */
int @_compile(const char *source, @_tape *out) {
  @_vw_label labels[256]; size_t label_count=0, count=0, total=0, pass;
  char token[128];
  if(!source || !out || out->size>out->capacity || (out->capacity && !out->data)) return -1;
  for(pass=0;pass<3;++pass) {
    const char *cursor=source; int got; count=0;
    while((got=@_vw_token(&cursor,token,sizeof(token)))>0) {
      size_t len=strlen(token), j; const @_metadata *info;
      if(len && token[len-1]==':') {
        token[len-1]=0; if(!token[0]) return -4;
        if(pass==0) {
          if(label_count==256) return -3;
          for(j=0;j<label_count;++j) if(strcmp(labels[j].name,token)==0) return -4;
          strcpy(labels[label_count].name,token); labels[label_count++].address=count;
        }
        continue;
      }
      info=@_vw_mnemonic(token); if(!info) return -4;
      if(count>=out->capacity) return -3;
      if(pass==2) {memset(&out->data[count],0,sizeof(out->data[count]));
        out->data[count].opcode=info->opcode; out->data[count].operand_count=info->operands;}
      for(j=0;j<info->operands;++j) {
        @_cell value=0; char *end; __PARSEDTYPE__ parsed;
        if(@_vw_token(&cursor,token,sizeof(token))!=1) return -4;
        if(token[0]=='&') {
          size_t k;
          if(pass) {for(k=0;k<label_count && strcmp(labels[k].name,token+1)!=0;++k) {} if(k==label_count) return -4; value=(@_cell)labels[k].address;}
        } else {
          errno=0; parsed=__STRTO__(token,&end,0);
          __RANGECHECK__
          value=(@_cell)parsed;
        }
        if(pass && @_vw_label_operand(info->opcode,j) && (uintmax_t)value>=total) return -4;
        if(pass==2) out->data[count].operands[j]=value;
      }
      ++count;
    }
    if(got<0) return -4;
    if(pass==0) total=count;
  }
  out->size=count; return 0;
}
]]
  text = qualify(text, name)
  local function substitute(marker, value)
    return (text:gsub(marker, function() return value end))
  end
  text = substitute("__MNEMONICS__", table.concat(mnemonics, "\n"))
  if module.cell == "uint64_t" then
    text = substitute("__PARSEDTYPE__", "uintmax_t")
    text = substitute("__STRTO__", "strtoumax")
    text = substitute("__RANGECHECK__",
      "if(token[0]=='-' || errno || *end || end==token || (uintmax_t)(" .. name
        .. "_cell)parsed!=parsed) return -4;")
  else
    text = substitute("__PARSEDTYPE__", "intmax_t")
    text = substitute("__STRTO__", "strtoimax")
    text = substitute("__RANGECHECK__",
      "if(errno || *end || end==token || (intmax_t)(" .. name .. "_cell)parsed!=parsed) return -4;")
  end
  return text
end

local function gen_rewrite(module)
  local name = module.name
  local opcode_of = {}
  for _, word in ipairs(module.words) do opcode_of[word.name] = word.opcode end
  local out = {}
  out[#out + 1] = "int " .. name .. "_rewrite(" .. name .. "_tape *tape) {"
  out[#out + 1] = "  size_t i=0,j,depth=0;"
  out[#out + 1] = "  if(!tape || tape->size > tape->capacity || (tape->size && !tape->data)) return -1;"
  out[#out + 1] = "  for(j=0;j<tape->size;++j) { const " .. name .. "_metadata *info=" .. name
    .. "_lookup(tape->data[j].opcode);"
  out[#out + 1] = "    if(" .. name .. "_vw_validate_instruction(&tape->data[j],tape->size)) return -4;"
  out[#out + 1] = "    if(strcmp(info->flow,\"next\") && (strcmp(info->flow,\"halt\") || j+1!=tape->size)) return -9;"
  out[#out + 1] = "    if(depth<info->inputs) return -2; depth=depth-info->inputs+info->outputs;"
  out[#out + 1] = "    if(depth>" .. module.stack_capacity .. ") return -3; }"
  out[#out + 1] = "  while(i<tape->size) {"
  for _, rule in ipairs(module.rewrites) do
    local condition = {}
    for index, mnemonic in ipairs(rule.from) do
      condition[#condition + 1] = "tape->data[i+" .. (index - 1) .. "].opcode==" .. opcode_of[mnemonic] .. "u"
    end
    out[#out + 1] = "    if(tape->size-i >= " .. #rule.from .. " && " .. table.concat(condition, " && ") .. ") {"
    for index, mnemonic in ipairs(rule.to) do
      out[#out + 1] = "      memset(&tape->data[i+" .. (index - 1) .. "],0,sizeof(tape->data[0]));"
        .. " tape->data[i+" .. (index - 1) .. "].opcode=" .. opcode_of[mnemonic] .. "u;"
    end
    out[#out + 1] = "      memmove(&tape->data[i+" .. #rule.to .. "], &tape->data[i+" .. #rule.from
      .. "], (tape->size-i-" .. #rule.from .. ")*sizeof(tape->data[0]));"
    out[#out + 1] = "      tape->size-=" .. (#rule.from - #rule.to) .. "; i=0; continue;"
    out[#out + 1] = "    }"
  end
  out[#out + 1] = "    ++i;"
  out[#out + 1] = "  }"
  out[#out + 1] = "  return 0;"
  out[#out + 1] = "}"
  return table.concat(out, "\n")
end

local optional_fragments = {
  tape = [[
int @_tape_validate(const @_tape *tape) {
  size_t i;
  if(!tape || tape->size>tape->capacity || (tape->size && !tape->data)) return -1;
  for(i=0;i<tape->size;++i) if(@_vw_validate_instruction(&tape->data[i],tape->size)) return -4;
  return 0;
}
]],
  memory = [[
int @_memory_set(@_vm *vm, @_allocator allocator) {
  if(!vm || !allocator.allocate || !allocator.release) return -1;
@guard  vm->vw_allocator=allocator; return 0;
}
void *@_alloc(@_vm *vm, size_t bytes) {
  void *value;
  if(!vm || vm->vw_error || !vm->vw_allocator.allocate) { @_vw_fail(vm,-8); return NULL; }
  value=vm->vw_allocator.allocate(vm->vw_allocator.context,bytes);
  if(!value && bytes) @_vw_fail(vm,-8);
  return value;
}
void *@_memory_resize(@_vm *vm, void *value, size_t bytes) {
  void *result;
  if(!vm || vm->vw_error || !vm->vw_allocator.reallocate) { @_vw_fail(vm,-8); return NULL; }
  result=vm->vw_allocator.reallocate(vm->vw_allocator.context,value,bytes);
  if(!result && bytes) @_vw_fail(vm,-8);
  return result;
}
void @_free(@_vm *vm, void *value) {
  if(vm && value && vm->vw_allocator.release) vm->vw_allocator.release(vm->vw_allocator.context,value);
}
]],
  frame = [[
int @_call(@_vm *vm, @_cell target) {
  @_activation *frame;
  if(!vm || vm->vw_error) return vm ? vm->vw_error : -1;
  if((uintmax_t)target>=vm->vw_code_size) { @_vw_fail(vm,-4); return -4; }
  if(vm->vw_frame_count>=sizeof(vm->vw_frames)/sizeof(vm->vw_frames[0])) { @_vw_fail(vm,-3); return -3; }
  frame=&vm->vw_frames[vm->vw_frame_count++]; memset(frame,0,sizeof(*frame));
  frame->return_address=vm->vw_pc; frame->stack_base=vm->vw_sp; vm->vw_pc=(size_t)target; return 0;
}
int @_ret(@_vm *vm) {
  if(!vm || vm->vw_error) return vm ? vm->vw_error : -1;
  if(!vm->vw_frame_count) { @_vw_fail(vm,-2); return -2; }
  vm->vw_pc=vm->vw_frames[--vm->vw_frame_count].return_address; return 0;
}
]],
  object = [[
@_object *@_object_new(@_vm *vm, uint32_t kind, size_t bytes) {
  @_object *object;
  if(bytes>SIZE_MAX-sizeof(*object)) { @_vw_fail(vm,-8); return NULL; }
  object=(@_object *)@_alloc(vm,sizeof(*object)+bytes); if(!object) return NULL;
  object->kind=kind; object->size=bytes; memset(object->data,0,bytes);
  object->next=vm->vw_objects; vm->vw_objects=object; return object;
}
int @_object_read(const @_object *object, size_t offset, void *out, size_t bytes) {
  if(!object || (bytes && !out) || offset>object->size || bytes>object->size-offset) return -4;
  if(bytes) memcpy(out,object->data+offset,bytes); return 0;
}
int @_object_write(@_object *object, size_t offset, const void *in, size_t bytes) {
  if(!object || (bytes && !in) || offset>object->size || bytes>object->size-offset) return -4;
  if(bytes) memcpy(object->data+offset,in,bytes); return 0;
}
int @_object_delete(@_vm *vm, @_object *object) {
  @_object **link;
  if(!vm || !object) return -1;
  for(link=&vm->vw_objects; *link && *link!=object; link=&(*link)->next) {}
  if(!*link) return -1;
  *link=object->next; @_free(vm,object); return 0;
}
int @_object_clear(@_vm *vm) {
  if(!vm) return -1;
  while(vm->vw_objects) { @_object *next=vm->vw_objects->next; @_free(vm,vm->vw_objects); vm->vw_objects=next; }
  return 0;
}
]],
  ipc = [[
int @_ipc_send(@_vm *vm, @_cell value) {
  size_t capacity;
  if(!vm) return -1;
  capacity=sizeof(vm->vw_messages)/sizeof(vm->vw_messages[0]);
  if(vm->vw_message_count==capacity) return -3;
  vm->vw_messages[(vm->vw_message_head+vm->vw_message_count++)%capacity]=value; return 0;
}
int @_ipc_receive(@_vm *vm, @_cell *value) {
  if(!vm || !value) return -1;
  if(!vm->vw_message_count) return -2;
  *value=vm->vw_messages[vm->vw_message_head]; --vm->vw_message_count;
  vm->vw_message_head=(vm->vw_message_head+1)%(sizeof(vm->vw_messages)/sizeof(vm->vw_messages[0])); return 0;
}
]],
  module = [[
void *@_module_find(const @_module *module, const char *name) {
  size_t i;
  if(!module || !name || (module->export_count && !module->exports)) return NULL;
  for(i=0;i<module->export_count;++i)
    if(module->exports[i].name && strcmp(module->exports[i].name,name)==0) return module->exports[i].address;
  return NULL;
}
int @_module_validate(const @_module *module) {
  size_t i,j;
  if(!module || !module->name || (module->export_count && !module->exports)) return -1;
  for(i=0;i<module->export_count;++i) {
    if(!module->exports[i].name) return -4;
    for(j=0;j<i;++j) if(strcmp(module->exports[i].name,module->exports[j].name)==0) return -4;
  }
  if(module->code) {
    const @_tape *tape=module->code;
    if(tape->size>tape->capacity || (tape->size && !tape->data)) return -4;
    for(i=0;i<tape->size;++i) if(@_vw_validate_instruction(&tape->data[i],tape->size)) return -4;
  }
  return 0;
}
int @_module_load(@_vm *vm, const @_module *module) {
  int status;
  if(!vm) return -1;
  status=@_module_validate(module); if(status) return status;
  vm->vw_module=module; vm->vw_pc=0; vm->vw_halted=0;
  vm->vw_code_size=module->code ? module->code->size : 0;
  return 0;
}
]],
  atomic = [[
@_atomic @_atomic_integer(@_cell value) { @_atomic out; memset(&out,0,sizeof(out)); out.tag=@_integer; out.value.integer=value; return out; }
@_atomic @_atomic_float(double value) { @_atomic out; memset(&out,0,sizeof(out)); out.tag=@_floating; out.value.floating=value; return out; }
@_atomic @_atomic_bool(int value) { @_atomic out; memset(&out,0,sizeof(out)); out.tag=@_boolean; out.value.boolean=!!value; return out; }
@_atomic @_atomic_reference(void *value) { @_atomic out; memset(&out,0,sizeof(out)); out.tag=@_reference; out.value.reference=value; return out; }
@_atomic @_atomic_nil(void) { @_atomic out; memset(&out,0,sizeof(out)); return out; }
]],
  optim = [[
void @_optim_invalidate(@_vm *vm) { if(vm) { memset(vm->vw_cache,0,sizeof(vm->vw_cache)); ++vm->vw_epoch; } }
void @_optim_store(@_vm *vm, uintptr_t key, void *value) {
  @_cache_entry *entry; if(!vm) return; entry=&vm->vw_cache[key%16]; entry->key=key; entry->value=value; entry->epoch=vm->vw_epoch;
}
void *@_optim_lookup(@_vm *vm, uintptr_t key) {
  @_cache_entry *entry; if(!vm) return NULL; entry=&vm->vw_cache[key%16];
  return entry->key==key && entry->epoch==vm->vw_epoch ? entry->value : NULL;
}
]],
  token = [[
const char *@_token(uint32_t opcode) { const @_metadata *info=@_lookup(opcode); return info ? info->name : NULL; }
]],
}

local system_allocator = [[
static void *@_vw_system_allocate(void *context, size_t bytes) { (void)context; return malloc(bytes); }
static void *@_vw_system_resize(void *context, void *p, size_t bytes) { (void)context; return realloc(p,bytes); }
static void @_vw_system_free(void *context, void *p) { (void)context; free(p); }
int @_memory_system(@_vm *vm) {
  @_allocator a={NULL,@_vw_system_allocate,@_vw_system_resize,@_vw_system_free}; return @_memory_set(vm,a);
}
]]

local function gen_fragment(module, component)
  local name = module.name
  local body
  if component == "insncode" then
    body = gen_handlers(module)
  elseif component == "srtbl" or component == "addrtbl" or component == "insntbl" then
    body = gen_table(module)
  elseif component == "dispatch" then
    body = gen_dispatch(module)
  elseif component == "compile" then
    body = gen_compiler(module)
  elseif component == "rewrite" then
    body = gen_rewrite(module)
  elseif component == "opcode" then
    local lines = {}
    for _, word in ipairs(module.words) do
      lines[#lines + 1] = "#define " .. name .. "_opcode_" .. word.name .. " " .. word.opcode .. "u"
    end
    body = table.concat(lines, "\n")
  elseif optional_fragments[component] then
    local text = optional_fragments[component]
    if component == "memory" then
      local guard = selected(module, "object") and "  if(vm->vw_objects) return -1;\n" or ""
      text = text:gsub("@guard", (guard:gsub("%%", "%%%%")))
    end
    body = qualify(text, name)
    if component == "memory" and module.allocator == "system" then
      body = body .. qualify(system_allocator, name)
    end
  else
    body = "/* component '" .. component .. "' has no body in this generator */"
  end
  local subsystem = module.subsystems[component]
  if subsystem then
    body = body .. "\n/* User subsystem extension. */\n" .. subsystem .. "\n"
  end
  return body
end

local function gen_body(module)
  local out = {}
  for _, component in ipairs(module.components) do
    out[#out + 1] = "/* ." .. component .. " */"
    out[#out + 1] = gen_fragment(module, component)
  end
  return table.concat(out, "\n")
end

-- Build the lowered module for a description, honouring per-call options.
local function module_for(vm, options)
  options = options or {}
  local hooks = vm.hooks
  if options.hooks ~= nil then hooks = options.hooks end
  local components = deep_copy(vm.components)
  local execution = options.dispatch or vm.execution
  if options.dispatch then
    assert(execution_models[options.dispatch] and options.dispatch ~= "none",
      "VMWeave: unknown dispatch model '" .. tostring(options.dispatch) .. "'")
    local tables = { subroutine = "srtbl", indirect = "addrtbl", direct = "insntbl" }
    components = { "insncode", "dispatch" }
    if tables[execution] then components[#components + 1] = tables[execution] end
  end
  local spec = spec_of(vm, components, execution, hooks)
  return lower(spec, check(spec))
end

function methods:emit(options)
  receiver(self)
  if options then check_keys(options, { dispatch = true, hooks = true }, "generation option") end
  local module = module_for(self, options)
  return gen_header(module) .. "\n" .. gen_body(module)
end

function methods:emit_header(options)
  receiver(self)
  return gen_header(module_for(self, options))
end

function methods:emit_source(options)
  receiver(self)
  local module = module_for(self, options)
  return "/* Generated component assembly. */\n#include \"" .. self.name .. ".h\"\n" .. gen_body(module)
end

-- STK-00 text interchange.  Deterministic and re-parsable by a conforming
-- reader; provided so generated VMs carry a machine-readable contract.
local function stk_quote(text)
  local out = { '"' }
  for index = 1, #text do
    local char = text:sub(index, index)
    if char == '"' or char == "\\" then out[#out + 1] = "\\" .. char
    else out[#out + 1] = char end
  end
  out[#out + 1] = '"'
  return table.concat(out)
end

function methods:emit_stk(options)
  receiver(self)
  local module = module_for(self, options)
  local out = {
    "stk-00 1",
    "vm " .. stk_quote(module.name),
    "configuration " .. stk_quote(module.execution) .. " " .. stk_quote(module.cell) .. " "
      .. module.stack_capacity .. " " .. module.frame_capacity .. " " .. module.ipc_capacity .. " "
      .. stk_quote(module.allocator) .. " " .. (module.hooks and 1 or 0),
    "source " .. stk_quote(module.source.file) .. " " .. module.source.line,
  }
  for _, component in ipairs(module.components) do
    out[#out + 1] = "component " .. stk_quote(component)
  end
  for _, field in ipairs(module.fields) do
    out[#out + 1] = "field " .. stk_quote(field.name) .. " " .. stk_quote(field.type)
      .. " " .. (field.count or 0)
  end
  local function body(text)
    return "c{ " .. #text .. "\n" .. text .. "\n}c"
  end
  for _, component in ipairs(sorted_keys(module.subsystems)) do
    out[#out + 1] = "subsystem " .. stk_quote(component) .. " " .. body(module.subsystems[component])
  end
  for _, word in ipairs(module.words) do
    local line = ": " .. stk_quote(word.name) .. " opcode " .. word.opcode
      .. " operands " .. #word.operands
    for _, operand in ipairs(word.operands) do line = line .. " " .. stk_quote(operand) end
    line = line .. " effect " .. stk_quote(word.effect) .. " flow " .. stk_quote(word.flow)
      .. " source " .. stk_quote(word.source.file) .. " " .. word.source.line
    out[#out + 1] = line
    out[#out + 1] = body(word.semantics) .. " ;"
  end
  for _, rule in ipairs(module.rewrites) do
    local line = "rewrite " .. #rule.from
    for _, mnemonic in ipairs(rule.from) do line = line .. " " .. stk_quote(mnemonic) end
    line = line .. " to " .. #rule.to
    for _, mnemonic in ipairs(rule.to) do line = line .. " " .. stk_quote(mnemonic) end
    out[#out + 1] = line
  end
  out[#out + 1] = "end"
  return table.concat(out, "\n") .. "\n"
end

-- Emit the three artifacts for a VM description as a name-keyed map.
function methods:artifacts(options)
  receiver(self)
  local module = module_for(self, options)
  local files = {}
  files[self.name .. ".h"] = gen_header(module)
  files[self.name .. ".stk00"] = self:emit_stk(options)
  local assembly = { "/* Generated component assembly. */", "#include \"" .. self.name .. ".h\"" }
  for _, component in ipairs(module.components) do
    assembly[#assembly + 1] = "#include \"" .. self.name .. "." .. component .. "\""
    files[self.name .. "." .. component] = "#include \"" .. self.name .. ".h\"\n" .. gen_fragment(module, component)
  end
  files[self.name .. ".c"] = table.concat(assembly, "\n") .. "\n"
  return files
end

return M
