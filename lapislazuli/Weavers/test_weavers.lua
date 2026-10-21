-- Run by test_collections.py with Weavers/*Weave on package.path.
local a, b = require("archweave"), require("bfdweave")
local here = debug.getinfo(1,"S").source:sub(2):match("^(.*)/[^/]*$")
local c = dofile(here .. "/common.lua")
local function bad(fn) assert(not pcall(fn), "expected rejection") end
local spec = {
  name="Edge", arch="test", machine=1, word_bits=32, address_bits=32, endian="big", source="test",
  instructions={ { name="edge", match=0, mask=0x80000000,
    operands={ { name="value", shift=0, bits=31, signed=true, scale=8 } } } },
}
local function reject(change)
  local copy = c.copy(spec); change(copy); bad(function() a.arch(copy) end)
end
reject(function(s) s.name="int" end)
reject(function(s) s.name="X\n*/" end)
reject(function(s) s.source="x\0" end)
reject(function(s) s.machine=1.5 end)
reject(function(s) s.endian="native" end)
reject(function(s) s.unknown=true end)
reject(function(s) s.instructions={} end)
reject(function(s) s.instructions[3]=s.instructions[1] end)
reject(function(s) s.instructions[1].mask=-1 end)
reject(function(s) s.instructions[1].match=1 end)
reject(function(s) s.instructions[1].name="bad-name" end)
reject(function(s) s.instructions[1].operands[1].bits=30 end)
reject(function(s) s.instructions[1].operands[1].bits=32 end)
reject(function(s) s.instructions[1].operands[1].shift=1 end)
reject(function(s) s.instructions[1].operands[1].scale=3 end)
reject(function(s) s.instructions[1].operands[1].signed="false" end)
reject(function(s) s.instructions[1].operands[2]=c.copy(s.instructions[1].operands[1]) end)
reject(function(s) s.instructions[2]=c.copy(s.instructions[1]); s.instructions[2].name="alias" end)
local edge = a.arch(spec)
spec.name="Changed"
assert(edge.name=="Edge", "constructor must snapshot input")
local file=assert(io.open(arg[1] .. "/Edge.h","wb")); file:write(edge:emit_header()); file:close()
file=assert(io.open(arg[1] .. "/Edge.c","wb")); file:write(edge:emit()); file:close()
for _, name in ipairs { "MIPS32", "PowerPC32", "SPARC32" } do
  local original = a.load(here .. "/../ArchCollection/" .. name .. "/" .. name .. ".archw")
  local reversed = c.copy(original)
  for i=1,#reversed.instructions//2 do
    local j=#reversed.instructions+1-i
    reversed.instructions[i],reversed.instructions[j]=reversed.instructions[j],reversed.instructions[i]
  end
  assert(a.arch(reversed):emit()==original:emit(), "instruction order must not affect emission")
end
local elf = { name="Test", target="test", arch="test", class=64, endian="little", machine=62, source="test" }
for _, change in ipairs {
  function(s) s.name="bad" end, function(s) s.class=128 end,
  function(s) s.endian="native" end, function(s) s.machine=0 end,
  function(s) s.machine=65536 end, function(s) s.machine=62.0 end,
  function(s) s.target="" end, function(s) s.source="\0" end,
  function(s) s.nope=true end,
} do
  local copy=c.copy(elf); change(copy); bad(function() b.elf(copy) end)
end
local target=b.elf(elf)
elf.target="changed"
assert(target.target=="test")
assert(target:emit()==target:emit() and target:emit_header()==target:emit_header())
print("ArchWeave/BFDWeave validation passed")
