-- Run under the native ASDL host with native.asdl loaded.
local P = asdl.lib.require("std.passes")
local R = asdl.lib.require("std.serialize")
local manager = P.New {
    {name="make", cache=true, effects={}, run=function()
        return asdl.make("root", nil, {name="café", items={
            asdl.make("value", "Num", {value=41}),
            asdl.make("value", "Flag", {value=false}),
        }})
    end},
    {name="rewrite", requires={"make"}, run=function(_, deps)
        -- Logistics annotates its input; preserve the cached AST's annotations.
        return asdl.logistics(asdl.clone(deps.make))
    end},
    {name="encode", requires={"rewrite"}, run=function(_, deps)
        return R.JSON(deps.rewrite)
    end},
}
local results, first = P.Run(manager, nil, {revision=1})
assert(results.encode:find('"value":42', 1, true))
assert(first.ran == 3 and first.hits == 0)
local again, second = P.Run(manager, nil, {revision=1})
assert(again.encode == results.encode)
assert(second.ran == 2 and second.hits == 1)
assert(asdl.annotation(again.make, "checked") == nil)

local D = asdl.lib.require("std.diagnostics")
local errors = asdl.diagnostic.count("error")
local warnings = asdl.diagnostic.count("warning")
local infos = asdl.diagnostic.count("info")
local mark = D.Checkpoint()
assert(D.IsEnabled("warning"))
D.Disable("warning")
assert(not D.IsEnabled("warning") and D.IsEnabled("info"))
assert(D.Warn(D.FaultPattern("W1", "%d")) == nil) -- muted messages are not formatted
assert(asdl.diagnostic.count("warning") == warnings)
D.Inform("info remains on")
assert(asdl.diagnostic.count("info") == infos + 1)

D.Disable()
assert(not D.IsEnabled("error") and not D.IsEnabled("info"))
assert(D.Inform("muted") == nil)
assert(asdl.diagnostic.count("info") == infos + 1)
assert(D.Error("muted error") == nil)
assert(D.FailedSince(mark))
local ok, err = pcall(D.Fatal, "muted fatal error")
assert(not ok and err == "ASDL semantic analysis aborted")
assert(D.Checkpoint() == mark + 2 and asdl.diagnostic.count("error") == errors)

D.Enable("warning")
assert(D.IsEnabled("warning") and not D.IsEnabled("error"))
D.Warn("warning restored")
assert(asdl.diagnostic.count("warning") == warnings + 1)
D.Enable()
assert(D.IsEnabled("warning") and D.IsEnabled("error") and D.IsEnabled("info"))
assert(not pcall(D.Disable, "typo"))

local binding = D.FaultPattern("W01", "Unused binding %s",
    {name = "bindings.unused", group = "bindings"})
local shadowing = D.FaultPattern("W01", "Shadowed binding %s",
    {name = "bindings.shadowed", group = "bindings"})
local io = D.FaultPattern("W01", "Missing input", {group = "io"})
local bare = D.FaultPattern("W01", "No metadata")
D.Disable({group = "bindings"})
D.Enable({name = "bindings.unused"})
assert(D.IsEnabled("warning", binding) and not D.IsEnabled("warning", shadowing))
assert(D.IsEnabled("warning", io) and D.IsEnabled("warning", bare))
local before = asdl.diagnostic.count("warning")
D.Warn(binding, nil, "x")
D.Warn(shadowing) -- no formatting of suppressed warnings
D.Warn(io)
assert(asdl.diagnostic.count("warning") == before + 2)
local records = asdl.diagnostic.records()
local named = records[#records - 1]
assert(named.name == "bindings.unused" and named.group == "bindings" and named.code == "W01")
assert(named.message == "Unused binding x")
assert(records[#records].group == "io" and records[#records].name == nil)

D.Disable("warning")
assert(D.IsEnabled("warning", binding) and not D.IsEnabled("warning", bare))
D.Enable({group = "bindings"})
D.Disable({name = "bindings.unused"})
assert(not D.IsEnabled("warning", binding) and D.IsEnabled("warning", shadowing))
D.Disable()
assert(not D.IsEnabled("warning", binding) and not D.IsEnabled("warning", shadowing))
D.Enable()
assert(D.IsEnabled("warning", binding) and D.IsEnabled("warning", shadowing))
assert(not pcall(D.Disable, {group = "bindings", name = "bindings.unused"}))
assert(not pcall(D.Enable, {group = ""}))
assert(not pcall(D.FaultPattern, "W01", "%s", {name = false}))
print("PASS native ASDL stdlib reflection, serialization, caching and diagnostic controls")
