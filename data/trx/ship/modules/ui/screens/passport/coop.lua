-- The passport's co-op lists: setting up a game to host or to join, typing a
-- name or an address, and the lobby the players wait in until the host starts.
-- Offered from the new game page of the title's passport.

local pages = require("common.ui.screens.passport.pages")
local requester = require("common.ui.widgets.requester")

local M = {}

trx.locale.declare({
  ["general/coop/host_game"] = "Host a co-op game",
  ["general/coop/join_game"] = "Join a co-op game",
  ["general/coop/name_fmt"] = "Name: %s",
  ["general/coop/address_fmt"] = "Host address: %s",
  ["general/coop/create"] = "Create the game",
  ["general/coop/join"] = "Join",
  ["general/coop/enter_name"] = "Type your name",
  ["general/coop/enter_address"] = "Type the host's address",
  ["general/coop/lobby_title"] = "Co-op game",
  ["general/coop/connecting_fmt"] = "Connecting to %s...",
  ["general/coop/waiting_players"] = "Waiting for players...",
  ["general/coop/waiting_host"] = "Waiting for the host to start...",
  ["general/coop/start"] = "Start a new game",
  ["general/coop/load"] = "Load a game",
  ["general/coop/leave"] = "Leave the co-op game",
  ["general/coop/player_fmt"] = "%d. %s",
  ["general/coop/player_host_fmt"] = "%d. %s (host)",
  ["general/coop/player_you_fmt"] = "%d. %s (you)",
  ["general/coop/player_away_fmt"] = "%d. %s (away)",
  ["general/coop/no_address"] = "none",
  ["general/coop/network_failed"] = "The network could not start.",
})

-- How often the lobby looks again at who is playing, in ticks.
local LOBBY_REFRESH_TICKS = 10
-- Where the co-op lists rest, as the passport's own lists do on the title.
local ANCHOR = 0.98

local function make_req(title, row_list)
  local req = requester.new({
    title = title,
    rows = row_list,
    row_pad = 20.0,
    arrows = false,
  })
  req.place = requester.on_page_line(req, ANCHOR, 0)
  return req
end

-- The co-op choices the new game page offers, on the title only.
function M.new_game_rows()
  if not pages.is_title() then
    return {}
  end
  return {
    { text = trx.locale.get("general/coop/host_game"), choice = "coop_host" },
    { text = trx.locale.get("general/coop/join_game"), choice = "coop_join" },
  }
end

-------------------------------------------------------------------------------
-- Typing a line
-------------------------------------------------------------------------------

-- Opens a list of one row that shows what is being typed. The keyboard is
-- the script's until the line is confirmed with Enter or dropped with Escape.
-- `on_done` gets the line, or nil when it was dropped.
local function push_prompt(passport, title, initial, max_len, on_done)
  local value = initial or ""
  local req = make_req(title, { { text = value .. "_" } })
  -- The keys reach the line through the events below, not through the list.
  local entry = passport:push_list("coop_prompt", req, { ignore_keys = true })
  local grab = trx.input.grab()
  local listeners = {}

  local function finish(result)
    for _, listener in ipairs(listeners) do
      listener:detach()
    end
    listeners = {}
    if grab ~= nil then
      grab:release()
      grab = nil
    end
    entry.layer:close()
    on_done(result)
  end

  local function show()
    req:set_rows({ { text = value .. "_" } })
  end

  listeners[#listeners + 1] = trx.events.on_text_input(function(text)
    if grab == nil then
      return
    end
    value = (value .. text):sub(1, max_len)
    show()
  end)

  local function on_key(key)
    if grab == nil then
      return
    end
    if key == "backspace" then
      value = value:sub(1, -2)
      show()
    elseif key == "return" or key == "keypad enter" then
      finish(value)
    elseif key == "escape" then
      finish(nil)
    end
  end
  listeners[#listeners + 1] = trx.events.on_key_down(on_key)
  listeners[#listeners + 1] = trx.events.on_key_repeat(on_key)
end

-------------------------------------------------------------------------------
-- Lobby
-------------------------------------------------------------------------------

local function player_text(player)
  local key = "general/coop/player_fmt"
  if not player.connected then
    key = "general/coop/player_away_fmt"
  elseif player.is_local then
    key = "general/coop/player_you_fmt"
  elseif player.number == 1 then
    key = "general/coop/player_host_fmt"
  end
  return trx.locale.format(key, player.number, player.name)
end

-- Whether any slot holds a save the host could go on from.
local function has_saves()
  local found = false
  pages.each_slot(function(slot_num, pool)
    found = found or trx.savegame.info(slot_num, pool) ~= nil
  end)
  return found
end

local function lobby_rows()
  local list = {}
  local status = trx.coop.status
  if status == "connecting" then
    list[#list + 1] = {
      text = trx.locale.format(
        "general/coop/connecting_fmt",
        trx.coop.last_address
      ),
    }
  else
    for _, player in ipairs(trx.coop.players()) do
      list[#list + 1] = { text = player_text(player) }
    end
    list[#list + 1] = {
      text = trx.locale.get(
        trx.coop.is_host and "general/coop/waiting_players"
          or "general/coop/waiting_host"
      ),
      rule = true,
    }
  end

  local actions = {}
  if trx.coop.is_host then
    actions[#actions + 1] = {
      text = trx.locale.get("general/coop/start"),
      action = "start",
    }
    if has_saves() then
      actions[#actions + 1] = {
        text = trx.locale.get("general/coop/load"),
        action = "load",
      }
    end
  end
  actions[#actions + 1] = {
    text = trx.locale.get("general/coop/leave"),
    action = "leave",
  }
  for index, row in ipairs(actions) do
    row.rule = row.rule or index == 1
    list[#list + 1] = row
  end
  return list
end

-- Puts the cursor on the first row that does something.
local function select_first_action(req)
  for index, row in ipairs(req.rows) do
    if row.action ~= nil then
      req.list:select(index)
      return
    end
  end
end

local function push_lobby(passport)
  local req = make_req(trx.locale.get("general/coop/lobby_title"), lobby_rows())
  select_first_action(req)
  local ticks = 0
  local self_entry
  self_entry = passport:push_list("coop_lobby", req, {
    on_tick = function()
      -- The host started: the passport makes way for the level.
      if trx.coop.start_pending then
        passport:close(true)
        return
      end
      -- The session is over, the host gone or the game full: the lobby
      -- has nothing left to wait for.
      if trx.coop.status == "off" then
        self_entry.layer:close()
        return
      end
      ticks = ticks + 1
      if ticks % LOBBY_REFRESH_TICKS ~= 0 then
        return
      end
      local selected = req:selected_row()
      local action = selected ~= nil and selected.action or nil
      req:set_rows(lobby_rows())
      for index, row in ipairs(req.rows) do
        if action ~= nil and row.action == action then
          req.list:select(index)
          return
        end
      end
      select_first_action(req)
    end,
    -- Backing out of the lobby leaves the game, as leaving does.
    on_cancel = function()
      trx.coop.leave()
    end,
  })
end

-------------------------------------------------------------------------------
-- Setting up
-------------------------------------------------------------------------------

local function setup_rows(kind)
  local list = {
    {
      text = trx.locale.format("general/coop/name_fmt", trx.coop.name),
      action = "name",
    },
  }
  if kind == "join" then
    local address = trx.coop.last_address
    if address == "" then
      address = trx.locale.get("general/coop/no_address")
    end
    list[#list + 1] = {
      text = trx.locale.format("general/coop/address_fmt", address),
      action = "address",
    }
  end
  list[#list + 1] = {
    text = trx.locale.get(
      kind == "host" and "general/coop/create" or "general/coop/join"
    ),
    action = kind,
    rule = true,
  }
  return list
end

local function push_setup(passport, kind)
  local title = trx.locale.get(
    kind == "host" and "general/coop/host_game" or "general/coop/join_game"
  )
  local req = make_req(title, setup_rows(kind))
  local entry = passport:push_list("coop_setup", req, { kind = kind })

  local function refresh(action)
    req:set_rows(setup_rows(kind))
    for index, row in ipairs(req.rows) do
      if row.action == action then
        req.list:select(index)
      end
    end
  end
  entry.coop_refresh = refresh
end

-- Runs the choice of a co-op list. Returns false for a list that is not one.
function M.choose(passport, entry, row)
  local role = entry.role
  if role == "coop_setup" then
    if row.action == "name" then
      push_prompt(
        passport,
        trx.locale.get("general/coop/enter_name"),
        trx.coop.name,
        15,
        function(value)
          if value ~= nil then
            trx.coop.name = value
          end
          entry.coop_refresh("name")
        end
      )
    elseif row.action == "address" then
      push_prompt(
        passport,
        trx.locale.get("general/coop/enter_address"),
        trx.coop.last_address,
        63,
        function(value)
          if value ~= nil and value ~= "" then
            trx.coop.last_address = value
          end
          entry.coop_refresh("address")
        end
      )
    elseif row.action == "host" then
      if trx.coop.host() then
        push_lobby(passport)
      else
        passport:show_message(trx.locale.get("general/coop/network_failed"))
      end
    elseif row.action == "join" then
      local address = trx.coop.last_address
      if address == "" then
        return true
      end
      if trx.coop.join(address) then
        push_lobby(passport)
      else
        passport:show_message(trx.locale.get("general/coop/network_failed"))
      end
    end
    return true
  elseif role == "coop_lobby" then
    if row.action == "start" then
      trx.game.start_new_game()
      passport:close(true)
    elseif row.action == "load" then
      -- Asked for here rather than at the top: the rows ask for this module.
      local rows = require("common.ui.screens.passport.rows")
      passport:push_list(
        "coop_load",
        make_req(
          trx.locale.get("general/passport/select_save"),
          rows.slot_rows()
        )
      )
    elseif row.action == "leave" then
      trx.coop.leave()
      entry.layer:close()
    end
    return true
  elseif role == "coop_load" then
    -- The others follow the host into the level the save is in.
    if trx.savegame.info(row.slot_num, row.pool) ~= nil then
      trx.savegame.load(row.slot_num, row.pool)
      passport:close(true)
    end
    return true
  elseif role == "coop_prompt" then
    return true
  end
  return false
end

-- Runs the choice of the new game page. Returns false for one that is not a
-- co-op choice.
function M.choose_new_game(passport, row)
  if row.choice == "coop_host" then
    push_setup(passport, "host")
    return true
  elseif row.choice == "coop_join" then
    push_setup(passport, "join")
    return true
  end
  return false
end

return M
