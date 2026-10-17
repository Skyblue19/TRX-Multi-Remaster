-- The co-op revive question: a large medipack used with a teammate down in
-- the level is not spent until the player says on whom, Lara herself or one
-- of the fallen.
--
-- Usage:
--   require("common.ui.coop_revive").setup()

local requester = require("common.ui.widgets.requester")

local M = {}

trx.locale.declare({
  ["general/coop/revive_title"] = "Large medipack",
  ["general/coop/heal_self"] = "Heal yourself",
  ["general/coop/revive_fmt"] = "Revive %s",
  ["general/coop/cancel"] = "Cancel",
})

-- Where the question sits on the screen, from the top.
local ANCHOR = 0.5

local function make_rows()
  local rows = {
    { text = trx.locale.get("general/coop/heal_self"), heal = true },
  }
  for _, player in ipairs(trx.coop.dead_players()) do
    rows[#rows + 1] = {
      text = trx.locale.format("general/coop/revive_fmt", player.name),
      number = player.number,
    }
  end
  rows[#rows + 1] = {
    text = trx.locale.get("general/coop/cancel"),
    rule = true,
  }
  return rows
end

local layer = nil

local function open()
  local req = requester.new({
    title = trx.locale.get("general/coop/revive_title"),
    rows = make_rows(),
    row_pad = 20.0,
    arrows = false,
    place = requester.anchored(ANCHOR),
  })
  -- A fallen player first, as that is what the question is for.
  if #req.rows > 2 then
    req.list:select(2)
  end
  layer = trx.ui.layers.push({
    root = req,
    place = req.place,
    on_input = function(self, keys)
      local picked = req:control(keys)
      if picked == nil then
        return
      end
      local row = picked ~= "cancel" and req.rows[picked] or nil
      if row ~= nil and row.heal then
        trx.coop.heal_self()
      elseif row ~= nil and row.number ~= nil then
        trx.coop.revive(row.number)
      else
        trx.coop.cancel_revive()
      end
      self:close()
    end,
    on_close = function()
      layer = nil
      -- Closed any other way, the medipack stays unspent.
      if trx.coop.revive_prompt then
        trx.coop.cancel_revive()
      end
    end,
  })
end

function M.setup()
  trx.events.on_tick(function()
    if trx.coop.revive_prompt and layer == nil then
      open()
    elseif not trx.coop.revive_prompt and layer ~= nil then
      layer:close()
    end
  end)
end

return M
