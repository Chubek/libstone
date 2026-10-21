-- Regenerate the C artifacts for every VM in this collection.
--
-- Each VM directory holds four files:
--
--   <Dir>.vmw    the VMWeave description (plus any shared Lua model it loads)
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
--   lua5.4 VMCollection/regenerate.lua --check   # reject stale/missing artifacts
--   lua5.4 VMCollection/regenerate.lua --list    # list available descriptions
--
-- Output is deterministic; regenerating an unchanged description rewrites
-- byte-identical files.

local function parent(path)
  return path:match("^(.*)[/\\][^/\\]*$")
end

local script = arg and arg[0] or "regenerate.lua"
local here = parent(script) or "."
local root = here .. "/.."

package.path = root .. "/Weavers/VMWeave/?.lua;" .. root .. "/Weavers/VMWeave/?/init.lua;" .. package.path
local vmweave = require("vmweave")

-- Discovery is structural: a directory is a VM when it has <Dir>/<Dir>.vmw.
local function listdir(path)
  local names = {}
  local pipe = io.popen("ls -1 '" .. path:gsub("'", "'\\''") .. "' 2>/dev/null")
  assert(pipe, "cannot discover descriptions in " .. path)
  for name in pipe:lines() do names[#names + 1] = name end
  assert(pipe:close(), "cannot list " .. path)
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
  assert(handle:write(text))
  assert(handle:close())
end

local wanted, checking, listing = {}, false, false
for index = 1, #arg do
  local option = arg[index]
  if option == "--check" then checking = true
  elseif option == "--list" then listing = true
  elseif option:sub(1, 1) == "-" then error("unknown option: " .. option)
  else wanted[option] = true end
end

-- Stable #line paths regardless of working directory or absolute invocation.
local function canonical_source(source)
  if not source or not source.file then return end
  local path = source.file
  assert(path:sub(1, #here + 1) == here .. "/", "source outside collection: " .. path)
  local parts = {}
  for part in path:sub(#here + 2):gmatch("[^/]+") do
    if part == ".." then assert(#parts > 0); table.remove(parts)
    elseif part ~= "." then parts[#parts + 1] = part end
  end
  source.file = "VMCollection/" .. table.concat(parts, "/")
end

local entries, known = {}, {}
for _, entry in ipairs(listdir(here)) do
  if not entry:match("^%.") and read_file(here .. "/" .. entry .. "/" .. entry .. ".vmw") then
    entries[#entries + 1] = entry; known[entry] = true
  end
end
for entry in pairs(wanted) do assert(known[entry], "unknown VM: " .. entry) end
assert(#entries > 0, "no VM descriptions found")
local selected = next(wanted) ~= nil

local generated, failed = 0, 0
for _, entry in ipairs(entries) do
  local spec_path = here .. "/" .. entry .. "/" .. entry .. ".vmw"
  if not selected or wanted[entry] then
    if listing then
      io.write(entry .. "\n")
    else
      local ok, description = pcall(function()
        local description = vmweave.load(spec_path)
        canonical_source(description.source)
        for _, field in ipairs(description.fields) do canonical_source(field.source) end
        for _, word in ipairs(description.instructions) do canonical_source(word.source) end
        local artifacts = { h = description:emit_header(), c = description:emit(), stk00 = description:emit_stk() }
        assert(description:emit_header() == artifacts.h and description:emit() == artifacts.c
          and description:emit_stk() == artifacts.stk00, entry .. ": non-deterministic emission")
        for _, extension in ipairs { "h", "c", "stk00" } do
          local path = here .. "/" .. entry .. "/" .. entry .. "." .. extension
          if checking then assert(read_file(path) == artifacts[extension], "stale or missing: " .. path)
          else write_file(path, artifacts[extension]) end
        end
        return description
      end)
      if not ok then
        io.stderr:write("FAIL " .. entry .. ": " .. tostring(description) .. "\n")
        failed = failed + 1
      else
        io.write(string.format("%s %-14s %-12s %2d instructions, %d components\n",
          checking and "checked" or "generated", entry, description.name, #description.instructions, #description.components))
        generated = generated + 1
      end
    end
  end
end

if not listing then io.write(string.format("%d %s, %d failed\n", generated, checking and "checked" or "generated", failed)) end
if failed > 0 then os.exit(1) end
