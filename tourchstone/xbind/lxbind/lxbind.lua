local M = {}

M.VERSION = "0.1.0"

local function fail(message)
  error("lxbind: " .. message, 3)
end

local function is_array(value)
  if type(value) ~= "table" then
    return false
  end
  local count = 0
  for key, _ in pairs(value) do
    if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
      return false
    end
    count = count + 1
  end
  for index = 1, count do
    if value[index] == nil then
      return false
    end
  end
  return true
end

local function list(value, field)
  if value == nil then
    return {}
  end
  if not is_array(value) then
    fail(field .. " must be an array")
  end
  return value
end

local function require_string(value, field)
  if type(value) ~= "string" or value == "" then
    fail(field .. " must be a non-empty string")
  end
  return value
end

local function copy_options(options)
  local result = {}
  if options == nil then
    return result
  end
  if type(options) ~= "table" then
    fail("options must be a table")
  end
  for key, value in pairs(options) do
    result[key] = value
  end
  return result
end

function M.target(name, options)
  local result = copy_options(options)
  result.kind = "target"
  result.name = require_string(name, "target name")
  result.features = list(result.features, "target.features")
  result.typedefs = list(result.typedefs, "target.typedefs")
  return result
end

function M.typedef(c_type, native_type, options)
  local result = copy_options(options)
  result.kind = "typedef"
  result.c = require_string(c_type, "typedef c type")
  result.native = require_string(native_type, "typedef native type")
  return result
end

function M.param(name, c_type, options)
  local result = copy_options(options)
  result.kind = "param"
  result.name = require_string(name, "param name")
  result.c = require_string(c_type, "param c type")
  return result
end

function M.func(name, options)
  local result = copy_options(options)
  result.kind = "function"
  result.name = require_string(name, "function name")
  result.returns = require_string(result.returns or "void", "function returns")
  result.params = list(result.params, "function.params")
  result.features = list(result.features, "function.features")
  return result
end

M["function"] = M.func

function M.module(name, options)
  local result = copy_options(options)
  result.kind = "module"
  result.name = require_string(name, "module name")
  result.functions = list(result.functions, "module.functions")
  result.typedefs = list(result.typedefs, "module.typedefs")
  return result
end

function M.library(options)
  local result = copy_options(options)
  result.kind = "library"
  result.name = require_string(result.name, "library name")
  result.headers = list(result.headers, "library.headers")
  result.targets = list(result.targets, "library.targets")
  result.typedefs = list(result.typedefs, "library.typedefs")
  result.functions = list(result.functions, "library.functions")
  result.modules = list(result.modules, "library.modules")
  result.stdlib = list(result.stdlib, "library.stdlib")
  result.version = result.version or M.VERSION
  return result
end

function M.validate_library(manifest)
  if type(manifest) ~= "table" or manifest.kind ~= "library" then
    fail("manifest must be produced by xbind.library")
  end
  require_string(manifest.name, "library name")
  for index, target in ipairs(manifest.targets or {}) do
    if type(target) ~= "table" or target.kind ~= "target" then
      fail("library.targets[" .. index .. "] must be produced by xbind.target")
    end
  end
  for index, fn in ipairs(manifest.functions or {}) do
    if type(fn) ~= "table" or fn.kind ~= "function" then
      fail("library.functions[" .. index .. "] must be produced by xbind.func")
    end
  end
  return manifest
end

return M
