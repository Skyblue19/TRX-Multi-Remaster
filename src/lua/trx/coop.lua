local raw = trxc.coop
local h = require("trx.internal.helpers")

---@class (partial) trx
---@field coop trx.coop

---Module for co-op play over the network.
---
---One player hosts and up to three others join, each under a name of their
---own. A session started here lasts until it is left, through every level the
---host goes to.
---
---```lua
---trx.coop.name = "Lara"
---trx.coop.host()
---```
---@trx.module 45 Co-op
---@class (exact) trx.coop
---@trx.readonly status, is_host, is_client, start_pending
---@field status string Where the session stands: `"off"`, `"hosting"`, `"connecting"` or `"connected"`.
---@field is_host boolean Whether this game hosts the session.
---@field is_client boolean Whether this game joined a session hosted elsewhere. A client neither saves nor loads: the host's saves are the ones that count.
---@field start_pending boolean Whether this game is on its way to the level the host went to. A screen open in front holds the move back, so a screen that sees this should close.
---@field name string This player's name, as the other players see it. Letters, digits, `-` and `_`, up to 15 of them; it is kept for the next session.
---@field last_address string The host this player last joined or typed, or `""` for none. It is kept for the next session.
local M = h.module("coop")

h.properties(M, "coop", {
  status = {
    get = raw.status,
  },
  is_host = {
    get = raw.is_host,
  },
  is_client = {
    get = raw.is_client,
  },
  start_pending = {
    get = raw.is_start_pending,
  },
  name = {
    get = raw.get_name,
    set = function(name)
      raw.set_name(name)
    end,
  },
  last_address = {
    get = raw.get_last_address,
    set = raw.set_last_address,
  },
})

---A player in the session.
---@class (exact) trx.coop.Player
---@field number integer The player's number, from 1, the host's being 1.
---@field name string The player's name.
---@field connected boolean Whether the player is playing now, rather than gone and expected back.
---@field is_local boolean Whether this is the player at this game.

---Starts hosting a session, ending any session already going.
---@return boolean # Whether the session started.
---@type fun(): boolean
M.host = raw.host

---Starts trying to join the session hosted at an address, ending any session
---already going. The address is kept as `last_address`.
---@param address string A host name or an IP address.
---@return boolean # Whether the network started; reaching the host takes longer.
---@type fun(address: string): boolean
M.join = raw.join

---Ends the session, letting the other players know.
---@type fun()
M.leave = raw.leave

---The players this game knows of, in order of their numbers.
---@return trx.coop.Player[]
---@type fun(): trx.coop.Player[]
M.players = raw.players
