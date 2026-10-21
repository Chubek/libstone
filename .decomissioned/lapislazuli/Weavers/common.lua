-- Shared validation/emission helpers for ArchWeave and BFDWeave (Lua 5.4).
local M = {}

function M.keys(value, allowed, what)
  assert(type(value) == "table", what .. " must be a table")
  for key in pairs(value) do assert(allowed[key], "unknown " .. what .. " attribute: " .. tostring(key)) end
end

function M.list(value, what)
  assert(type(value) == "table", what .. " must be a list")
  local count = 0
  for key in pairs(value) do
    assert(math.type(key) == "integer" and key >= 1 and key <= #value, what .. " must be dense")
    count = count + 1
  end
  assert(count == #value, what .. " must be dense")
  return value
end

function M.integer(value, low, high, what)
  assert(math.type(value) == "integer" and value >= low and value <= high, "invalid " .. what)
  return value
end

function M.text(value, what)
  assert(type(value) == "string" and #value > 0 and not value:find("%z"), "invalid " .. what)
  return value
end

function M.name(value)
  assert(type(value) == "string" and value:match("^[A-Z][A-Za-z0-9_]*$"),
    "C namespace must start with an uppercase letter")
  return value
end

function M.copy(value)
  if type(value) ~= "table" then return value end
  local result = {}
  for key, item in pairs(value) do result[key] = M.copy(item) end
  return result
end

function M.quote(value)
  return '"' .. value:gsub('[%z\1-\31\127-\255\\"]', function(c)
    return string.format("\\%03o", c:byte())
  end) .. '"'
end

function M.read(path)
  local file = assert(io.open(path, "rb"))
  local result = assert(file:read("a")); assert(file:close())
  return result
end

function M.render(text, values)
  return (text:gsub("@([A-Z_]+)@", function(key) return assert(values[key], "missing template key " .. key) end))
end

function M.load(path, constructor, metatable)
  local result = assert(loadfile(path, "t"))()
  assert(getmetatable(result) == metatable, "description must return a " .. constructor .. " object")
  return result
end

-- Structural discovery and canonical emission, independent of the invocation cwd.
function M.regenerate(script, collection, extension, module)
  local here = script:match("^(.*)/[^/]*$") or "."
  local root = here .. "/.."
  package.path = root .. "/Weavers/" .. module .. "/?.lua;" .. package.path
  local weave = require(module)
  local wanted, checking, listing = {}, false, false
  for index = 1, #arg do
    local option = arg[index]
    if option == "--check" then checking = true
    elseif option == "--list" then listing = true
    elseif option:sub(1, 1) == "-" then error("unknown option: " .. option)
    else wanted[option] = true end
  end
  local pipe = assert(io.popen("ls -1 '" .. here:gsub("'", "'\\''") .. "'"))
  local names, known = {}, {}
  for entry in pipe:lines() do
    if entry:match("^[A-Z][A-Za-z0-9_]*$") then
      local file = io.open(here .. "/" .. entry .. "/" .. entry .. "." .. extension, "rb")
      if file then file:close(); names[#names + 1] = entry; known[entry] = true end
    end
  end
  assert(pipe:close(), "cannot list " .. here)
  table.sort(names)
  assert(#names > 0, "no descriptions in " .. collection)
  for name in pairs(wanted) do assert(known[name], "unknown profile: " .. name) end
  for _, name in ipairs(names) do
    if not next(wanted) or wanted[name] then
      if listing then io.write(name .. "\n") else
        local prefix = here .. "/" .. name .. "/" .. name
        local description = weave.load(prefix .. "." .. extension)
        assert(description.name == name, "directory and description names must agree")
        local artifacts = { h = description:emit_header(), c = description:emit() }
        assert(artifacts.h == description:emit_header() and artifacts.c == description:emit(),
          "non-deterministic emission: " .. name)
        for _, suffix in ipairs { "h", "c" } do
          local path = prefix .. "." .. suffix
          if checking then
            local file = io.open(path, "rb")
            local actual = file and file:read("a"); if file then file:close() end
            assert(actual == artifacts[suffix], "stale or missing: " .. path)
          else
            local file = assert(io.open(path, "wb"))
            assert(file:write(artifacts[suffix])); assert(file:close())
          end
        end
        io.write((checking and "checked " or "generated ") .. name .. "\n")
      end
    end
  end
end

return M
