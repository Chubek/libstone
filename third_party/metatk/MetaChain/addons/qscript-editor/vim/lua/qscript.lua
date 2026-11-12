-- qscript.lua: Neovim integration for QScript.
--
--   require('qscript').setup({
--     executable    = 'qscript',          -- the qscript driver
--     library_paths = { '~/qslib' },      -- extra -L directories
--     lsp = { cmd = { 'qscript-lsp' } },  -- false to disable the language server
--   })
--
-- Registers the filetype, starts QScript-LSP for qscript buffers (via
-- vim.lsp.start, no plugin needed), and defines :QScriptRun, :QScriptCheck,
-- :QScriptAst, :QScriptTree, :QScriptIR, :QScriptCall, :QScriptBuild,
-- :QScriptLib and :QScriptLspRestart. See :help qscript-lua.

local M = {}

M.config = {
  executable = 'qscript',
  library_paths = {},
  lsp = {
    -- Command that starts QScript-LSP. If 'qscript-lsp' is not on $PATH, the
    -- server shipped next to this runtime directory is tried with node.
    cmd = nil,
    check_on_change = true,
    debounce_ms = 250,
    on_attach = nil,
    capabilities = nil,
  },
  output = 'split',         -- 'split' (scratch window) or 'notify'
  mappings = true,          -- <LocalLeader>r/c/a/i in qscript buffers
}

local function merge(dst, src)
  for k, v in pairs(src or {}) do
    if type(v) == 'table' and type(dst[k]) == 'table' and not (vim.islist or vim.tbl_islist)(v) then
      merge(dst[k], v)
    else
      dst[k] = v
    end
  end
  return dst
end

local function expand_paths(paths)
  local out = {}
  for _, p in ipairs(paths or {}) do
    table.insert(out, vim.fn.expand(p))
  end
  return out
end

-- The qscript command line with -L options for the current buffer.
function M.command(subcommand, ...)
  local cmd = { M.config.executable }
  local file = vim.api.nvim_buf_get_name(0)
  local dirs = expand_paths(M.config.library_paths)
  if file ~= '' then
    table.insert(dirs, 1, vim.fn.fnamemodify(file, ':p:h'))
  end
  for _, dir in ipairs(dirs) do
    vim.list_extend(cmd, { '-L', dir })
  end
  table.insert(cmd, subcommand)
  vim.list_extend(cmd, { ... })
  return cmd
end

local function show(title, text, failed)
  local lines = vim.split(text or '', '\n', { trimempty = true })
  if #lines == 0 then
    lines = { '(no output)' }
  end
  if M.config.output == 'notify' then
    vim.notify(table.concat(lines, '\n'), failed and vim.log.levels.ERROR or vim.log.levels.INFO, { title = title })
    return
  end
  local buf
  for _, b in ipairs(vim.api.nvim_list_bufs()) do
    if vim.api.nvim_buf_is_valid(b) and vim.b[b].qscript_output then
      buf = b
    end
  end
  if not buf then
    buf = vim.api.nvim_create_buf(false, true)
    vim.b[buf].qscript_output = true
    vim.bo[buf].bufhidden = 'hide'
  end
  vim.bo[buf].modifiable = true
  vim.api.nvim_buf_set_lines(buf, 0, -1, false, lines)
  vim.bo[buf].modifiable = false
  local win = vim.fn.bufwinid(buf)
  if win == -1 then
    local current = vim.api.nvim_get_current_win()
    vim.cmd('botright ' .. math.min(math.max(#lines, 3), 15) .. 'split')
    vim.api.nvim_win_set_buf(0, buf)
    vim.wo.winbar = title
    vim.api.nvim_set_current_win(current)
  else
    vim.wo[win].winbar = title
  end
end

-- Run `qscript SUBCOMMAND FILE ARGS...` on the current buffer (saved first).
function M.run(subcommand, args)
  args = args or {}
  local file = vim.api.nvim_buf_get_name(0)
  if file == '' then
    vim.notify('qscript: buffer has no file name', vim.log.levels.WARN)
    return
  end
  if vim.bo.modified then
    vim.cmd('silent write')
  end
  local cmd = M.command(subcommand, file, (table.unpack or unpack)(args))
  vim.system(cmd, { text = true }, function(result)
    vim.schedule(function()
      local text = (result.stdout or '') .. (result.stderr or '')
      if result.code ~= 0 then
        text = text .. '\n[exit ' .. result.code .. ']'
      end
      show('qscript ' .. subcommand, text, result.code ~= 0)
    end)
  end)
end

local function server_cmd()
  local cfg = M.config.lsp
  if cfg.cmd then
    return cfg.cmd
  end
  if vim.fn.executable('qscript-lsp') == 1 then
    return { 'qscript-lsp' }
  end
  -- Look for the bundled server: <dir of this file>/../../lsp/server/index.js
  -- (the layout of the qscript editor archive) or ../lsp/server/index.js.
  local here = debug.getinfo(1, 'S').source:sub(2)
  local base = vim.fn.fnamemodify(here, ':p:h:h')
  for _, candidate in ipairs({ base .. '/../lsp/server/index.js', base .. '/lsp/server/index.js' }) do
    if vim.uv.fs_stat(candidate) then
      return { 'node', vim.fn.fnamemodify(candidate, ':p') }
    end
  end
  return { 'qscript-lsp' }
end

function M.lsp_settings()
  return {
    executable = M.config.executable,
    libraryPaths = expand_paths(M.config.library_paths),
    checkOnChange = M.config.lsp.check_on_change,
    debounceMs = M.config.lsp.debounce_ms,
  }
end

-- Start (or reuse) the language server for a buffer.
function M.start_lsp(bufnr)
  if M.config.lsp == false then
    return
  end
  bufnr = bufnr or vim.api.nvim_get_current_buf()
  local file = vim.api.nvim_buf_get_name(bufnr)
  local root = vim.fs.root and vim.fs.root(bufnr, { '.git', 'CMakeLists.txt', '.qscript' }) or nil
  if not root then
    root = file ~= '' and vim.fn.fnamemodify(file, ':p:h') or vim.uv.cwd()
  end
  local settings = M.lsp_settings()
  return vim.lsp.start({
    name = 'qscript',
    cmd = server_cmd(),
    root_dir = root,
    init_options = settings,
    settings = { qscript = settings },
    capabilities = M.config.lsp.capabilities,
    on_attach = M.config.lsp.on_attach,
  }, { bufnr = bufnr })
end

local function set_mappings(bufnr)
  if not M.config.mappings then
    return
  end
  local map = function(lhs, rhs, desc)
    vim.keymap.set('n', lhs, rhs, { buffer = bufnr, silent = true, desc = desc })
  end
  map('<LocalLeader>r', function() M.run('run') end, 'QScript: run')
  map('<LocalLeader>c', function() M.run('check') end, 'QScript: check')
  map('<LocalLeader>a', function() M.run('ast') end, 'QScript: AST')
  map('<LocalLeader>i', function() M.run('ir') end, 'QScript: MetaIL')
end

local function define_commands(bufnr)
  local cmd = function(name, fn, opts)
    vim.api.nvim_buf_create_user_command(bufnr, name, fn, opts or {})
  end
  cmd('QScriptRun', function(o) M.run('run', o.fargs) end, { nargs = '*' })
  cmd('QScriptCheck', function() M.run('check') end)
  cmd('QScriptAst', function() M.run('ast') end)
  cmd('QScriptTree', function() M.run('tree') end)
  cmd('QScriptIR', function() M.run('ir') end)
  cmd('QScriptCall', function(o) M.run('call', vim.list_extend({ '--' }, o.fargs)) end, { nargs = '+' })
  cmd('QScriptBuild', function(o) M.run('build', { '-o', vim.fn.expand(o.args) }) end, { nargs = 1, complete = 'file' })
  cmd('QScriptLib', function(o)
    vim.system({ M.config.executable, 'lib', o.args }, { text = true }, function(r)
      vim.schedule(function() show('qscript lib ' .. o.args, r.stdout ~= '' and r.stdout or r.stderr, r.code ~= 0) end)
    end)
  end, {
    nargs = 1,
    complete = function(lead)
      local out = vim.fn.systemlist({ M.config.executable, 'libs' })
      return vim.tbl_filter(function(n) return vim.startswith(n, lead) end, out)
    end,
  })
  cmd('QScriptLspRestart', function()
    for _, client in ipairs(vim.lsp.get_clients({ name = 'qscript' })) do
      vim.lsp.stop_client(client.id)
    end
    vim.defer_fn(function() M.start_lsp(bufnr) end, 200)
  end)
end

function M.attach(bufnr)
  bufnr = bufnr or vim.api.nvim_get_current_buf()
  define_commands(bufnr)
  set_mappings(bufnr)
  M.start_lsp(bufnr)
end

function M.setup(opts)
  merge(M.config, opts or {})
  if opts and opts.lsp == false then
    M.config.lsp = false
  end
  vim.filetype.add({
    extension = { qs = 'qscript', qscript = 'qscript' },
    pattern = {
      ['.*'] = {
        function(_, bufnr)
          local first = vim.api.nvim_buf_get_lines(bufnr, 0, 1, false)[1] or ''
          if first:match('^#!.*%f[%w]qscript%f[%W]') then
            return 'qscript'
          end
        end,
        { priority = -math.huge },
      },
    },
  })
  local group = vim.api.nvim_create_augroup('qscript', { clear = true })
  vim.api.nvim_create_autocmd('FileType', {
    group = group,
    pattern = 'qscript',
    callback = function(args) M.attach(args.buf) end,
  })
  -- Buffers that were already open when setup() ran.
  for _, b in ipairs(vim.api.nvim_list_bufs()) do
    if vim.api.nvim_buf_is_loaded(b) and vim.bo[b].filetype == 'qscript' then
      M.attach(b)
    end
  end
  return M
end

return M
