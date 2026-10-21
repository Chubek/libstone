-- Regenerate the C artifacts for every VM in this collection.
--
-- Each VM directory holds three files:
--
--   <Dir>.vmw    the VMWeave description (hand written, the source of truth)
--   <Dir>.h      the generated public interface
--   <Dir>.c      the generated self-contained implementation
--   <Dir>.stk00  the generated STK-00 interchange form
--
-- The `.c` translation unit is self-contained: it inlines the declarations
-- that also appear in the `.h`, so it can be compiled on its own.  Both carry
-- `#line` directives pointing back into the `.vmw`, so diagnostics and
-- debuggers resolve to the description that produced them.
--
--   lua5.4 VMCollection/regenerate.lua            # every VM
--   lua5.4 VMCollection/regenerate.lua ART FORTH  # selected VMs
--
-- Output is deterministic; regenerating an unchanged description rewrites
-- byte-identical files.

local function parent(path)
  return path:match("^(.*)[/\\][^/\\]*$")
end

local script = arg and arg[0] or "regenerate.lua"
local here = parent(script) or "."
local root = parent(here) or "."

package.path = root .. "/VMWeave/?.lua;" .. root .. "/VMWeave/?/init.lua;" .. package.path
local vmweave = require("vmweave")

-- Directory entries, in the order the collection documents them.  Discovery is
-- still structural (a directory is a VM when it holds `<Dir>/<Dir>.vmw`).
local function listdir(path)
  local names = {}
  local pipe = io.popen("ls -1 '" .. path:gsub("'", "'\\''") .. "' 2>/dev/null")
  if not pipe then return names end
  for name in pipe:lines() do names[#names + 1] = name end
  pipe:close()
  table.sort(names)
  return names
end

local function read_file(path)
  local handle = io.open(path, "rb")
  if not handle then return nil end
  local text = handle:read("a")
  handle:close()
  return text
end

local function write_file(path, text)
  local handle, message = io.open(path, "wb")
  assert(handle, message)
  handle:write(text)
  handle:close()
end

local wanted = {}
for index = 1, #arg do wanted[arg[index]] = true end

local generated, failed = 0, 0
for _, entry in ipairs(listdir(here)) do
  local spec_path = here .. "/" .. entry .. "/" .. entry .. ".vmw"
  local spec = not entry:match("^%.") and read_file(spec_path)
  if spec and (#arg == 0 or wanted[entry]) then
    local ok, description = pcall(vmweave.load, spec_path)
    if not ok then
      io.stderr:write("FAIL " .. entry .. ": " .. tostring(description) .. "\n")
      failed = failed + 1
    else
      local header = description:emit_header()
      local source = description:emit()
      local interchange = description:emit_stk()
      -- A regenerated description must round-trip to identical C.
      assert(description:emit() == source, entry .. ": non-deterministic emission")
      write_file(here .. "/" .. entry .. "/" .. entry .. ".h", header)
      write_file(here .. "/" .. entry .. "/" .. entry .. ".c", source)
      write_file(here .. "/" .. entry .. "/" .. entry .. ".stk00", interchange)
      io.write(string.format("generated %-14s %-12s %2d instructions, %d components\n",
        entry, description.name, #description.instructions, #description.components))
      generated = generated + 1
    end
  end
end

io.write(string.format("%d generated, %d failed\n", generated, failed))
if failed > 0 then os.exit(1) end
