local here = (arg[0]:match("^(.*)/[^/]*$") or ".")
local common = dofile(here .. "/../Weavers/common.lua")
common.regenerate(arg[0], "BFDCollection", "bfdw", "BFDWeave")
