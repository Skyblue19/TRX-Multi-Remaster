#include <trx/game/coop/common.h>
#include <trx/game/coop/identity.h>
#include <trx/game/coop/net.h>
#include <trx/game/lua/registry.h>
#include <trx/game/lua/utils/module.h>

#include <lauxlib.h>

// trxc.coop.status(): string
// "off", "hosting", "connecting" or "connected".
static int M_L_CoopStatus(lua_State *const L)
{
    switch (Coop_Net_GetStatus()) {
    case COOP_NET_HOSTING:
        lua_pushstring(L, "hosting");
        break;
    case COOP_NET_CONNECTING:
        lua_pushstring(L, "connecting");
        break;
    case COOP_NET_CONNECTED:
        lua_pushstring(L, "connected");
        break;
    default:
        lua_pushstring(L, "off");
        break;
    }
    return 1;
}

// trxc.coop.is_host(): boolean
static int M_L_CoopIsHost(lua_State *const L)
{
    lua_pushboolean(L, Coop_Net_IsHost());
    return 1;
}

// trxc.coop.is_client(): boolean
static int M_L_CoopIsClient(lua_State *const L)
{
    lua_pushboolean(L, Coop_IsClient());
    return 1;
}

// trxc.coop.is_start_pending(): boolean
static int M_L_CoopIsStartPending(lua_State *const L)
{
    lua_pushboolean(L, Coop_Net_IsStartPending());
    return 1;
}

// trxc.coop.get_name(): string
static int M_L_CoopGetName(lua_State *const L)
{
    lua_pushstring(L, Coop_Identity_Get()->name);
    return 1;
}

// trxc.coop.set_name(name: string): boolean
static int M_L_CoopSetName(lua_State *const L)
{
    lua_pushboolean(L, Coop_Identity_SetName(luaL_checkstring(L, 1)));
    return 1;
}

// trxc.coop.get_last_address(): string
static int M_L_CoopGetLastAddress(lua_State *const L)
{
    lua_pushstring(L, Coop_Identity_GetLastAddress());
    return 1;
}

// trxc.coop.set_last_address(address: string)
static int M_L_CoopSetLastAddress(lua_State *const L)
{
    Coop_Identity_SetLastAddress(luaL_checkstring(L, 1));
    return 0;
}

// trxc.coop.host(): boolean
static int M_L_CoopHost(lua_State *const L)
{
    lua_pushboolean(L, Coop_Net_Host());
    return 1;
}

// trxc.coop.join(address: string): boolean
static int M_L_CoopJoin(lua_State *const L)
{
    lua_pushboolean(L, Coop_Net_Join(luaL_checkstring(L, 1)));
    return 1;
}

// trxc.coop.leave()
static int M_L_CoopLeave(lua_State *const L)
{
    Coop_Net_Leave();
    return 0;
}

// trxc.coop.players(): { number: integer, name: string, connected: boolean,
// is_local: boolean }[]
static int M_L_CoopPlayers(lua_State *const L)
{
    COOP_PLAYER_INFO players[COOP_MAX_PLAYERS];
    const int32_t count = Coop_Net_GetPlayers(players);
    lua_createtable(L, count, 0);
    for (int32_t i = 0; i < count; i++) {
        const COOP_PLAYER_INFO *const player = &players[i];
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, player->number);
        lua_setfield(L, -2, "number");
        lua_pushstring(L, player->name);
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, player->connected);
        lua_setfield(L, -2, "connected");
        lua_pushboolean(L, player->is_local);
        lua_setfield(L, -2, "is_local");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// trxc.coop.is_revive_prompt_open(): boolean
static int M_L_CoopIsRevivePromptOpen(lua_State *const L)
{
    lua_pushboolean(L, Coop_IsRevivePromptOpen());
    return 1;
}

// trxc.coop.close_revive_prompt()
static int M_L_CoopCloseRevivePrompt(lua_State *const L)
{
    Coop_CloseRevivePrompt();
    return 0;
}

// trxc.coop.dead_players(): { number: integer, name: string }[]
static int M_L_CoopDeadPlayers(lua_State *const L)
{
    COOP_PLAYER_INFO players[COOP_MAX_PLAYERS];
    const int32_t count = Coop_Net_GetDeadTeammates(players);
    lua_createtable(L, count, 0);
    for (int32_t i = 0; i < count; i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, players[i].number);
        lua_setfield(L, -2, "number");
        lua_pushstring(L, players[i].name);
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// trxc.coop.revive(number: integer): boolean
static int M_L_CoopRevive(lua_State *const L)
{
    lua_pushboolean(
        L, Coop_ReviveWithMedipack((int32_t)luaL_checkinteger(L, 1)));
    return 1;
}

// trxc.coop.heal_self()
static int M_L_CoopHealSelf(lua_State *const L)
{
    Coop_HealWithMedipack();
    return 0;
}

static const luaL_Reg m_Module[] = {
    { "is_revive_prompt_open", M_L_CoopIsRevivePromptOpen },
    { "close_revive_prompt", M_L_CoopCloseRevivePrompt },
    { "dead_players", M_L_CoopDeadPlayers },
    { "revive", M_L_CoopRevive },
    { "heal_self", M_L_CoopHealSelf },
    { "status", M_L_CoopStatus },
    { "is_host", M_L_CoopIsHost },
    { "is_start_pending", M_L_CoopIsStartPending },
    { "is_client", M_L_CoopIsClient },
    { "get_name", M_L_CoopGetName },
    { "set_name", M_L_CoopSetName },
    { "get_last_address", M_L_CoopGetLastAddress },
    { "set_last_address", M_L_CoopSetLastAddress },
    { "host", M_L_CoopHost },
    { "join", M_L_CoopJoin },
    { "leave", M_L_CoopLeave },
    { "players", M_L_CoopPlayers },
    { nullptr, nullptr },
};

static void M_Create(lua_State *const L)
{
    LUA_RegisterModule(L, "coop", m_Module);
}

REGISTER_LUA_CAPI(.create = M_Create)
