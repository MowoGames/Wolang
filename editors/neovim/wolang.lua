-- Wolang for Neovim 0.10 or later.
--
-- Two things: `.wo` is Wolang, and Wolang files get a language server.
-- Everything a person sees comes from `wolang lsp` -- the compiler
-- answering -- so there is nothing here to keep in step with it.
--
-- Put this file in `~/.config/nvim/lua/wolang.lua` and `require('wolang')`
-- from `init.lua`, or copy the two blocks into your own configuration.

local M = {}

function M.setup(options)
  options = options or {}
  local command = options.command or 'wolang'

  vim.filetype.add({ extension = { wo = 'wolang' } })

  vim.api.nvim_create_autocmd('FileType', {
    pattern = 'wolang',
    callback = function(event)
      -- Two spaces, no tabs, and eighty columns: what the formatter the
      -- server runs already does, said again so that typing matches it.
      vim.bo[event.buf].expandtab = true
      vim.bo[event.buf].shiftwidth = 2
      vim.bo[event.buf].tabstop = 2
      vim.bo[event.buf].commentstring = '// %s'
      vim.bo[event.buf].textwidth = 80

      vim.lsp.start({
        name = 'wolang',
        cmd = { command, 'lsp' },
        root_dir = vim.fs.root(event.buf, { 'wolang.toml', '.git' }),
      })
    end,
  })

  -- `:WolangCheck` and friends, because the four things a person does to a
  -- package should not need the flags remembered.
  for _, task in ipairs({ 'check', 'test', 'build', 'run' }) do
    vim.api.nvim_create_user_command(
      'Wolang' .. task:sub(1, 1):upper() .. task:sub(2),
      function(args)
        local where = args.args ~= '' and args.args
          or vim.fs.root(0, { 'wolang.toml' })
          or '.'
        vim.cmd('botright split | terminal ' .. command .. ' ' .. task ..
                ' ' .. where)
      end,
      { nargs = '?', complete = 'dir' }
    )
  end
end

return M
