local cc = require("lprofcc")
local vm = require("lprofvm")

cc.on("end", function(event)
    -- Times/identities are exact decimal strings. Explicit numeric conversion
    -- is appropriate for this deliberately coarse heuristic threshold.
    if tonumber(event.duration_ns) > 10000000 then
        cc.finding("note", "CC-HOT-PASS", "Investigate pass: " .. event.name)
    end
end)

vm.on("error", function(event)
    vm.finding("error", "VM-OBSERVED-FAULT", event.name .. ": " .. event.detail)
end)

vm.on("counter", function(event)
    if event.name == "stack.depth" and event.value > 1000 then
        vm.finding("warning", "VM-DEEP-STACK", "Consider stack growth or recursion limits")
    end
end)
