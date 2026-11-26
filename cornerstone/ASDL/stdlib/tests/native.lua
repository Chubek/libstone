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
print("PASS native ASDL stdlib reflection, AST serialization/rewrite and pass caching")
