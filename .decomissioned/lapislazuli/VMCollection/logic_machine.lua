-- Shared WAM/PROL reference, integer and list unification engine.
-- Terms are 1-based heap handles. Variables point at zero (unbound) or another
-- handle. No general functors, occurs check, indexing, GC or source compiler.
-- Cyclic terms are outside the profile; dereference/unification work is bounded.
return function(name, words)
  local v = require("vmweave")
  local function c(text) return (text:gsub("@", name)) end
  local vm = v.vm {
    name = name, execution = "switch", stack = { cell = "int64_t", capacity = 16 },
    frame_capacity = 64,
    components = { "token", "opcode", "insncode", "dispatch", "tape", "compile", "frame" },
    fields = {
      { name = "x", type = "i64", count = 16 },
      { name = "kind", type = "u8", count = 1024 },
      { name = "a", type = "i64", count = 1024 }, { name = "b", type = "i64", count = 1024 },
      { name = "heap_top", type = "u64" },
      { name = "trail", type = "u64", count = 2048 }, { name = "trail_top", type = "u64" },
      { name = "choice_pc", type = "i64", count = 32 },
      { name = "choice_heap", type = "u64", count = 32 },
      { name = "choice_trail", type = "u64", count = 32 },
      { name = "choice_frames", type = "u64", count = 32 },
      { name = "choice_returns", type = "u64", count = 2048 },
      { name = "choice_x", type = "i64", count = 512 },
      { name = "choice_top", type = "u64" }, { name = "out", type = "i64" },
    },
    subsystems = { token = c [[
static int @_vw_reg(@_vm *vm, int64_t r) {
  if(r < 0 || r >= 16) { @_vw_fail(vm,-4); return 0; } return 1;
}
static int64_t @_vw_term(@_vm *vm, uint8_t kind, int64_t a, int64_t b) {
  size_t i;
  if(vm->heap_top >= 1024u) { @_vw_fail(vm,-3); return 0; }
  i = vm->heap_top++; vm->kind[i] = kind; vm->a[i] = a; vm->b[i] = b; return (int64_t)i + 1;
}
static int64_t @_vw_deref(@_vm *vm, int64_t term) {
  size_t steps = 0;
  for(;;) {
    size_t i;
    if(term <= 0 || (uint64_t)term > vm->heap_top) { @_vw_fail(vm,-4); return 0; }
    i = (size_t)term - 1u;
    if(vm->kind[i] != 0 || !vm->a[i]) return term;
    if(++steps > 1024u) { @_vw_fail(vm,-4); return 0; }
    term = vm->a[i];
  }
}
static void @_vw_backtrack(@_vm *vm) {
  size_t c, i;
  if(!vm->choice_top) { @_vw_fail(vm,-11); return; }
  c = vm->choice_top - 1u;
  while(vm->trail_top > vm->choice_trail[c]) {
    size_t var = vm->trail[--vm->trail_top]; vm->a[var] = 0;
  }
  vm->heap_top = vm->choice_heap[c]; memcpy(vm->x,&vm->choice_x[c * 16u],sizeof(vm->x));
  vm->vw_frame_count = vm->choice_frames[c];
  for(i = 0; i < vm->vw_frame_count; ++i) {
    vm->vw_frames[i].return_address = vm->choice_returns[c * 64u + i];
    vm->vw_frames[i].stack_base = 0;
  }
  @_vw_jump(vm,vm->choice_pc[c]);
}
/* Worklist unification; all bindings since the last choice are trailed. */
static void @_vw_unify(@_vm *vm, int64_t left, int64_t right) {
  int64_t ls[1024], rs[1024]; size_t top = 1, steps = 0;
  ls[0] = left; rs[0] = right;
  while(top && !vm->vw_error) {
    int64_t l, r; size_t li, ri;
    if(++steps > 4096u) { @_vw_fail(vm,-4); return; }
    --top; l = @_vw_deref(vm,ls[top]); r = @_vw_deref(vm,rs[top]);
    if(vm->vw_error) return;
    if(l == r) continue;
    li = (size_t)l - 1u; ri = (size_t)r - 1u;
    if(vm->kind[li] == 0 || vm->kind[ri] == 0) {
      size_t var = vm->kind[li] == 0 ? li : ri;
      int64_t value = vm->kind[li] == 0 ? r : l;
      if(vm->choice_top) {
        if(vm->trail_top >= 2048u) { @_vw_fail(vm,-3); return; }
        vm->trail[vm->trail_top++] = var;
      }
      vm->a[var] = value;
    } else if(vm->kind[li] != vm->kind[ri]
      || (vm->kind[li] == 1 && vm->a[li] != vm->a[ri])) {
      @_vw_backtrack(vm); return;
    } else if(vm->kind[li] == 2) {
      if(top + 2u > 1024u) { @_vw_fail(vm,-3); return; }
      ls[top] = vm->a[li]; rs[top++] = vm->a[ri];
      ls[top] = vm->b[li]; rs[top++] = vm->b[ri];
    }
  }
}
]] },
  }
  local function add(key, body, operands, flow)
    vm:instruction(words[key] or key, { semantics = c(body), operands = operands, flow = flow })
  end
  add("PUT_VARIABLE", [[
if(!@_vw_reg(vm,operand0)) return;
vm->x[(size_t)operand0] = @_vw_term(vm,0,0,0);]], { "cell" })
  add("PUT_INTEGER", [[
if(!@_vw_reg(vm,operand0)) return;
vm->x[(size_t)operand0] = @_vw_term(vm,1,operand1,0);]], { "cell", "cell" })
  add("PUT_NIL", [[
if(!@_vw_reg(vm,operand0)) return;
vm->x[(size_t)operand0] = @_vw_term(vm,3,0,0);]], { "cell" })
  add("PUT_VALUE", [[
if(!@_vw_reg(vm,operand0) || !@_vw_reg(vm,operand1)) return;
vm->x[(size_t)operand0] = vm->x[(size_t)operand1];]], { "cell", "cell" })
  add("PUT_LIST", [[
int64_t a, b;
if(!@_vw_reg(vm,operand0) || !@_vw_reg(vm,operand1) || !@_vw_reg(vm,operand2)) return;
a = vm->x[(size_t)operand1]; b = vm->x[(size_t)operand2];
if(a <= 0 || (uint64_t)a > vm->heap_top || b <= 0 || (uint64_t)b > vm->heap_top) { vm_fail(-4); return; }
vm->x[(size_t)operand0] = @_vw_term(vm,2,a,b);]], { "cell", "cell", "cell" })
  add("GET_VALUE", [[
if(!@_vw_reg(vm,operand0) || !@_vw_reg(vm,operand1)) return;
@_vw_unify(vm,vm->x[(size_t)operand0],vm->x[(size_t)operand1]);]], { "cell", "cell" }, "branch")
  add("GET_INTEGER", [[
int64_t term;
if(!@_vw_reg(vm,operand0)) return;
term = @_vw_term(vm,1,operand1,0); if(vm->vw_error) return;
@_vw_unify(vm,vm->x[(size_t)operand0],term);]], { "cell", "cell" }, "branch")
  add("TRY_ME_ELSE", [[
size_t c, i;
if(vm->choice_top >= 32u) { vm_fail(-3); return; }
c = vm->choice_top++; vm->choice_pc[c] = operand0;
vm->choice_heap[c] = vm->heap_top; vm->choice_trail[c] = vm->trail_top;
vm->choice_frames[c] = vm->vw_frame_count;
memcpy(&vm->choice_x[c * 16u],vm->x,sizeof(vm->x));
for(i = 0; i < vm->vw_frame_count; ++i) vm->choice_returns[c * 64u + i] = vm->vw_frames[i].return_address;]], { "label" })
  add("RETRY_ME_ELSE", [[
if(!vm->choice_top) { vm_fail(-2); return; }
vm->choice_pc[vm->choice_top - 1u] = operand0;]], { "label" })
  add("TRUST_ME", [[
if(!vm->choice_top) { vm_fail(-2); return; }
--vm->choice_top; if(!vm->choice_top) vm->trail_top = 0;]])
  add("CUT", "vm->choice_top = 0; vm->trail_top = 0;")
  add("FAIL", "@_vw_backtrack(vm);", nil, "branch")
  add("CALL", "(void)vm_call(operand0);", { "label" }, "call")
  add("EXECUTE", "vm_jump(operand0);", { "label" }, "jump")
  add("PROCEED", "(void)vm_return();", nil, "return")
  add("PRINT", [[
int64_t term;
if(!@_vw_reg(vm,operand0)) return;
term = @_vw_deref(vm,vm->x[(size_t)operand0]); if(vm->vw_error) return;
if(vm->kind[(size_t)term - 1u] != 1) { vm_fail(-4); return; }
vm->out = vm->a[(size_t)term - 1u];]], { "cell" })
  add("HALT", "vm_halt();", nil, "halt")
  return vm
end
