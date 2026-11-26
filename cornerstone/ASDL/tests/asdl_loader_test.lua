local fixtures, stdlib = ...
local lib = asdl.lib

local function fails(fragment, f, ...)
    local ok, message = pcall(f, ...)
    assert(not ok and type(message) == "string" and message:find(fragment, 1, true),
           ("expected %s, got %s"):format(fragment, tostring(message)))
end

local roots = lib.searchpath()
assert(#roots == 3 and roots[1] == fixtures.."/first" and
       roots[2] == fixtures.."/second" and roots[3] == stdlib)
roots[1] = "changed"
assert(lib.searchpath()[1] == fixtures.."/first")
assert(#lib.loaded() == 0)

local path = lib.resolve("pkg.base")
assert(path:sub(-#"/first/pkg/base.lua") == "/first/pkg/base.lua")
assert(lib.resolve("pkg/base.lua") == path)
assert(lib.require(path) == lib.require("pkg.base", "1.2.3"))
local base = lib.require("pkg/base")
assert(base.Label == "first" and base.Run == 1 and lib.issigned(base))
assert(base == lib.require("pkg/../pkg/base.lua"))
assert(base == lib.require("pkg_alias"))
assert(asdl.diagnostic.sentinel == "original")
assert(math.abs(-1) == 1)
assert(_G.loader_test_module_global == nil)
local explicit_root = fixtures:sub(1,1) == "/" and fixtures or "./"..fixtures
assert(lib.require(explicit_root.."/second/pkg/base.lua").Label == "second")
assert(lib.require("only").label == "second-root")
fails("missing ASDL library", lib.resolve, "no.such.library")
fails("regular file", lib.resolve, "not_file")
fails("nonempty string", lib.resolve, "bad\0name")
fails("invalid dotted", lib.resolve, "pkg..base")
fails("version mismatch", lib.require, "pkg.base", "1.0")
fails("undeclared", lib.require, "only", "1.0")
fails("sealed", function() base.Label = "changed" end)
fails("sealed", function() base.Added = true end)
assert(not pcall(setmetatable, base, {}))
local ticket, pair_state = pairs(base)
assert(type(ticket) == "function" and pair_state == nil)
local keys = lib.exports(base)
assert(table.concat(keys, ",") == "Label,Run")
local seen = {}
for k, v in pairs(base) do seen[k] = v end
assert(seen.Label == "first" and seen.Run == 1)
assert(lib.version(base) == "1.2.3")
fails("signed", lib.version, base, "1.2.4")

local child = lib.require("pkg.child")
local desc = lib.describe(child)
assert(desc.signed and desc.name == "pkg.base:derived" and desc.path == lib.resolve("pkg.child"))
assert(desc.parent == path and desc.version == "3")
assert(#desc.dependencies == 2 and desc.dependencies[1] == path and
       desc.dependencies[2] == lib.resolve("only"))
assert(child.Label == "derived" and child.Run == 1 and base.Label == "first")
desc.dependencies[1] = "changed"
assert(lib.describe(child).dependencies[1] == path)
assert(not lib.issigned({}) and not lib.issigned(false))
fails("not an ASDL library", asdl.signlib, {})

local fresh = lib.new("fresh")
fresh.value = true
asdl.signlib(fresh)
assert(fresh.value and lib.describe(fresh).path == nil)
fails("already signed", asdl.signlib, fresh)
local derived = lib.derive("pkg.base")
assert(lib.describe(derived).dependencies[1] == path and derived.Label == "first")
local invalid = lib.new("invalid")
invalid[1] = false
fails("invalid ASDL library export", asdl.signlib, invalid)
assert(invalid[1] == false and not lib.issigned(invalid))
invalid[1] = nil
invalid.valid = true
assert(asdl.signlib(invalid).valid)

fails("expected library prologue", lib.require, "missing_prologue")
fails("expected library trailer", lib.require, "missing_trailer")
fails("library must return its original LIB", lib.require, "wrong_identity")
fails("library must return signed LIB", lib.require, "fake_sign")
fails("circular ASDL library dependency", lib.require, "cycle_a")
fails("cycle_b.lua", lib.require, "cycle_a")
fails("boom 2", lib.require, "runtime_error")
fails("boom 3", lib.require, "runtime_error")
assert(not table.concat(lib.loaded(), "\n"):find("cycle_"))
assert(not table.concat(lib.loaded(), "\n"):find("runtime_error"))
assert(lib.require("pkg.base").Run == 1)
assert(lib.require("comments").ok)

for _, mod in ipairs {
    "analysis.abstractinterp", "analysis.callgraph", "analysis.dataflow", "analysis.defuse",
    "analysis.escape", "analysis.liveness", "analysis.reachingdefs", "analysis.usecounts",
    "cfg", "collections.bitset", "collections.multimap", "collections.orderedmap",
    "collections.priorityqueue", "collections.set", "collections.vector", "collections.worklist",
    "control", "diagnostics", "format.dot", "format.html", "format.mermaid", "format.pretty",
    "graph.directed", "graph.dominators", "graph.loops", "graph.reachability", "graph.scc",
    "graph.shortestpath", "graph.topology", "ir", "match.decisiontree", "match.structural",
    "match.unification", "rewrite", "scope", "semantics.bindings", "semantics.closures",
    "semantics.controlflow", "semantics.effects", "semantics.environment", "semantics.inheritance",
    "semantics.patterns", "serialize", "testing.assertions", "testing.fixtures", "testing.property",
    "testing.snapshot", "transform.anf", "transform.closureconv", "transform.constfold",
    "transform.cps", "transform.deadcode", "transform.inline", "transform.lambdalift",
    "transform.ssa", "tree", "types.constraints", "types.hm", "types.lattice", "types.subtyping",
    "types.unification",
} do
    assert(lib.issigned(lib.require("std."..mod)), mod)
end
assert(lib.require("std/serialize.lua").JSON({x=1}) == '{"x":1}')
local loaded = lib.loaded()
assert(#loaded > 60)
for i = 2, #loaded do assert(loaded[i-1] < loaded[i]) end
