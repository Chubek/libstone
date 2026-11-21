-- LLSynth source checking and tool generation for Neovim.
-- No third-party Neovim plugins or external Lua modules are required.
local M = {}

M.config = {
  -- An argv prefix, not a shell command. Configure to point to your checkout.
  command = { 'llsynth.pl' },
  mode = 'interpret', -- interpret | compile | rewrite | both
  target = 'python3', -- python3 | c99 | c11 | cpp17 | cpp20
  include_paths = {},
  module_paths = {},
  open_quickfix_on_error = true,
}

local extensions = {
  python3 = '.py',
  c99 = '.c',
  c11 = '.c',
  cpp17 = '.cpp',
  cpp20 = '.cpp',
}

local function notice(message, level)
  vim.notify(message, level or vim.log.levels.INFO, { title = 'LLSynth' })
end

function M.setup(options)
  M.config = vim.tbl_deep_extend('force', M.config, options or {})
end

local function command_prefix()
  local command = M.config.command
  if type(command) == 'string' then
    command = { command }
  end
  if type(command) ~= 'table' or #command == 0 then
    return nil, 'Configure require("llsynth").setup({command={...}}) first'
  end
  local args = {}
  for _, value in ipairs(command) do
    if type(value) ~= 'string' or value == '' then
      return nil, 'LLSynth command must be a nonempty argv list'
    end
    args[#args + 1] = value
  end
  if vim.fn.executable(args[1]) ~= 1 then
    return nil, 'Cannot execute ' .. args[1] .. ' (configure command in setup())'
  end
  return args
end

local function spec_path()
  local buffer = vim.api.nvim_get_current_buf()
  local filename = vim.api.nvim_buf_get_name(buffer)
  if filename == '' or not filename:match('%.llsynth$') then
    return nil, 'Open a saved .llsynth specification first'
  end
  if vim.bo[buffer].modified then
    return nil, 'Save the .llsynth buffer before running the generator'
  end
  if vim.fn.filereadable(filename) ~= 1 then
    return nil, 'Cannot read the .llsynth file on disk'
  end
  return filename
end

local function common_arguments(argv)
  for _, path in ipairs(M.config.include_paths or {}) do
    argv[#argv + 1] = '--include-path'
    argv[#argv + 1] = path
  end
  for _, path in ipairs(M.config.module_paths or {}) do
    argv[#argv + 1] = '--module-path'
    argv[#argv + 1] = path
  end
  return argv
end

local function run(argv, source, label, output)
  local chunks = {}
  local function collect(_, lines)
    for _, line in ipairs(lines or {}) do
      if line ~= '' then
        chunks[#chunks + 1] = line
      end
    end
  end

  local job = vim.fn.jobstart(argv, {
    cwd = vim.fn.fnamemodify(source, ':h'),
    stdout_buffered = true,
    stderr_buffered = true,
    on_stdout = collect,
    on_stderr = collect,
    on_exit = function(_, code)
      vim.schedule(function()
        if code == 0 then
          notice(label .. ' succeeded' .. (output and (': ' .. output) or ''))
          return
        end
        local lines = chunks
        if #lines == 0 then
          lines = { 'LLSynth exited with status ' .. tostring(code) }
        end
        vim.fn.setqflist({}, ' ', {
          title = label,
          lines = lines,
          efm = '%f:%l:%c: %m,%f:%l: %m,%m',
        })
        if M.config.open_quickfix_on_error then
          vim.cmd('copen')
        end
        notice(label .. ' failed (exit ' .. tostring(code) .. ')', vim.log.levels.ERROR)
      end)
    end,
  })
  if job <= 0 then
    notice('Could not start LLSynth (jobstart returned ' .. tostring(job) .. ')', vim.log.levels.ERROR)
  end
  return job
end

function M.check()
  local source, err = spec_path()
  if not source then
    notice(err, vim.log.levels.WARN)
    return nil
  end
  local argv, command_err = command_prefix()
  if not argv then
    notice(command_err, vim.log.levels.ERROR)
    return nil
  end
  argv[#argv + 1] = '--check'
  argv[#argv + 1] = '--mode=' .. M.config.mode
  common_arguments(argv)
  argv[#argv + 1] = source
  return run(argv, source, 'LLSynthCheck')
end

function M.generate(opts)
  opts = opts or {}
  local source, err = spec_path()
  if not source then
    notice(err, vim.log.levels.WARN)
    return nil
  end
  local target = opts.target
  if not target or target == '' then
    target = M.config.target
  end
  local extension = extensions[target]
  if not extension then
    notice('Unknown target ' .. tostring(target), vim.log.levels.ERROR)
    return nil
  end
  local argv, command_err = command_prefix()
  if not argv then
    notice(command_err, vim.log.levels.ERROR)
    return nil
  end
  local output = source:gsub('%.llsynth$', extension)
  argv[#argv + 1] = '--mode=' .. M.config.mode
  argv[#argv + 1] = '-T'
  argv[#argv + 1] = target
  argv[#argv + 1] = '-o'
  argv[#argv + 1] = output
  common_arguments(argv)
  argv[#argv + 1] = source
  return run(argv, source, 'LLSynthGenerate', output)
end

return M
