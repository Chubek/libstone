-- Shared bounded object/message model for the SOM and Smalltalk-80 profiles.
-- Not an image interpreter: selectors/classes are integer IDs, methods have
-- one argument, inheritance and block/non-local returns are not modelled.
return function(name, words)
  local v = require("vmweave")
  local function c(text) return (text:gsub("@", name)) end
  local vm = v.vm {
    name = name, execution = "switch", stack = { cell = "int64_t", capacity = 256 },
    frame_capacity = 64,
    components = { "token", "opcode", "insncode", "dispatch", "tape", "compile", "frame" },
    fields = {
      { name = "objects", type = "i64", count = 1024 },
      { name = "classes", type = "i64", count = 64 }, { name = "object_top", type = "u64" },
      { name = "method_class", type = "i64", count = 64 },
      { name = "method_selector", type = "i64", count = 64 },
      { name = "method_code", type = "i64", count = 64 }, { name = "method_top", type = "u64" },
      { name = "locals", type = "i64", count = 1040 },
      { name = "self", type = "i64", count = 65 }, { name = "depth", type = "u64" },
      { name = "out", type = "i64" },
    },
    subsystems = { token = c [[
static int64_t @_vw_bits(uint64_t x) {
  return x <= INT64_MAX ? (int64_t)x : -1 - (int64_t)(UINT64_MAX - x);
}
static int64_t @_vw_int(uint64_t n) { return @_vw_bits((n << 3) | 1u); }
static int64_t @_vw_value(int64_t n) {
  return n >= 0 ? n / 8 : -1 - (int64_t)((~(uint64_t)n) >> 3);
}
static int @_vw_object(@_vm *vm, int64_t ref) {
  uint64_t index = (uint64_t)ref >> 3;
  if(((uint64_t)ref & 7u) != 2u || index >= vm->object_top) { @_vw_fail(vm,-4); return 0; }
  return 1;
}
]] },
  }
  local function add(key, effect, body, operands, flow)
    vm:instruction(words[key] or key, { stack_effect = effect, semantics = c(body),
      operands = operands, flow = flow })
  end
  add("PUSH_INT", "( -- i )", "vm_push(@_vw_int((uint64_t)operand0));", { "cell" })
  add("PUSH_NIL", "( -- nil )", "vm_push(0);")
  add("DUP", "( a -- a a )", "cell a = vm_pop(); vm_push(a); vm_push(a);")
  add("POP", "( a -- )", "(void)vm_pop();")
  add("NEW", "( -- object )", [[
size_t i;
if(operand0 < 0) { vm_fail(-4); return; }
if(vm->object_top >= 64u) { vm_fail(-3); return; }
i = vm->object_top++; vm->classes[i] = operand0; vm_push((cell)((i << 3) | 2u));]], { "cell" })
  add("PUSH_SELF", "( -- receiver )", "vm_push(vm->self[vm->depth]);")
  add("PUSH_LOCAL", "( -- value )", [[
if(operand0 < 0 || operand0 >= 16) { vm_fail(-4); return; }
vm_push(vm->locals[vm->depth * 16u + (size_t)operand0]);]], { "cell" })
  add("POP_LOCAL", "( value -- )", [[
if(operand0 < 0 || operand0 >= 16) { vm_fail(-4); return; }
vm->locals[vm->depth * 16u + (size_t)operand0] = vm_pop();]], { "cell" })
  add("PUSH_FIELD", "( -- value )", [[
cell self = vm->self[vm->depth];
if(operand0 < 0 || operand0 >= 16 || !@_vw_object(vm,self)) { vm_fail(-4); return; }
vm_push(vm->objects[((uint64_t)self >> 3) * 16u + (size_t)operand0]);]], { "cell" })
  add("POP_FIELD", "( value -- )", [[
cell self = vm->self[vm->depth];
if(operand0 < 0 || operand0 >= 16 || !@_vw_object(vm,self)) { vm_fail(-4); return; }
vm->objects[((uint64_t)self >> 3) * 16u + (size_t)operand0] = vm_pop();]], { "cell" })
  add("METHOD", "( -- )", [[
size_t i;
if(operand0 < 0 || operand1 < 0) { vm_fail(-4); return; }
for(i = 0; i < vm->method_top; ++i)
  if(vm->method_class[i] == operand0 && vm->method_selector[i] == operand1) break;
if(i == vm->method_top) {
  if(i >= 64u) { vm_fail(-3); return; } ++vm->method_top;
}
vm->method_class[i] = operand0; vm->method_selector[i] = operand1; vm->method_code[i] = operand2;]],
    { "cell", "cell", "label" })
  add("SEND", "( receiver arg -- )", [[
cell arg = vm_pop(), self = vm_pop(), klass; size_t i;
if(!@_vw_object(vm,self)) return;
klass = vm->classes[(uint64_t)self >> 3];
for(i = 0; i < vm->method_top; ++i)
  if(vm->method_class[i] == klass && vm->method_selector[i] == operand0) break;
if(i == vm->method_top) { vm_fail(-4); return; } /* message not understood */
if(vm->depth >= 64u) { vm_fail(-3); return; }
if(vm_call(vm->method_code[i])) return;
++vm->depth; vm->self[vm->depth] = self;
memset(&vm->locals[vm->depth * 16u], 0, 16u * sizeof(int64_t));
vm->locals[vm->depth * 16u] = arg;]], { "cell" }, "call")
  add("RETURN", "( value -- value )", [[
cell result;
if(!vm->depth || !vm->vw_frame_count) { vm_fail(-2); return; }
result = vm_pop(); --vm->depth; (void)vm_return(); vm_push(result);]], nil, "return")
  for _, op in ipairs { { "ADD", "+" }, { "SUB", "-" }, { "MUL", "*" } } do
    add(op[1], "( a b -- c )", [[
cell b = vm_pop(), a = vm_pop();
if(((uint64_t)a & 7u) != 1u || ((uint64_t)b & 7u) != 1u) { vm_fail(-4); return; }
vm_push(@_vw_int((uint64_t)@_vw_value(a) ]] .. op[2] .. [[ (uint64_t)@_vw_value(b)));]])
  end
  add("JUMP", "( -- )", "vm_jump(operand0);", { "label" }, "jump")
  add("JUMP_IF_NIL", "( value -- )", "if(!vm_pop()) vm_jump(operand0);", { "label" }, "branch")
  add("PRINT", "( value -- )", [[
cell value = vm_pop();
if(((uint64_t)value & 7u) != 1u) { vm_fail(-4); return; }
vm->out = @_vw_value(value);]])
  add("HALT", "( -- )", "vm_halt();", nil, "halt")
  return vm
end
