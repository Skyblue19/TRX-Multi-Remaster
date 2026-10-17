#include <trx/game/coop/net.h>

#include <trx/config.h>
#include <trx/core/log.h>
#include <trx/core/memory.h>
#include <trx/core/subsystem.h>
#include <trx/core/utils.h>
#include <trx/debug.h>
#include <trx/game/console.h>
#include <trx/game/coop/common.h>
#include <trx/game/coop/creatures.h>
#include <trx/game/coop/identity.h>
#include <trx/game/coop/profile.h>
#include <trx/game/coop/puppet.h>
#include <trx/game/coop/wire.h>
#include <trx/game/coop/world.h>
#include <trx/game/game.h>
#include <trx/game/game/control.h>
#include <trx/game/game_flow.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/lara/cheat.h>
#include <trx/game/rooms.h>
#include <trx/game/savegame/manager.h>
#include <trx/game/shell.h>
#include <trx/game/ui/dialogs/takeover.h>

#include <enet/enet.h>

#include <stdint.h>
#include <string.h>

// Bumped whenever a packet changes shape. A peer on another version is turned
// away when it connects, rather than misreading every packet after.
#define M_PROTOCOL_VERSION 14
#define M_CHANNEL_COUNT 2
// Player states: unreliable, as each one replaces the last.
#define M_CHANNEL_STATE 0
// Everything else: reliable and in order, as each builds on the last.
#define M_CHANNEL_RELIABLE 1
// How often a game not yet caught up with the host's level asks again.
#define M_WORLD_REQUEST_FRAMES 30
// The host is player 0, and every player has a puppet slot of the same
// number in the others' games.
#define M_MAX_PLAYERS COOP_MAX_PLAYERS
static_assert(COOP_MAX_PLAYERS <= COOP_MAX_PUPPETS, "a puppet per player");
#define M_HOST_INDEX 0
#define M_NO_PLAYER 0xFF
// A level loads in one go, with nothing taken in meanwhile, so a peer is only
// given up on after longer than any load takes.
#define M_PEER_TIMEOUT_MIN_MS 30000
#define M_PEER_TIMEOUT_MAX_MS 60000
// How often a client that lost its host tries it again.
#define M_RECONNECT_FRAMES 90
// How often a client looks whether its profile changed, sending it only when
// it did: a few hundred bytes every five seconds at most.
#define M_PROFILE_FRAMES 150
// How many players a host's save keeps, the ones playing now and the ones
// who were playing when it was saved.
#define M_MAX_SAVED_PROFILES 8
// How long a client leaving waits for the host to take its last profile.
#define M_LEAVE_WAIT_MS 1000

typedef enum {
    // A player's state, sent by its game and passed on by the host.
    M_PACKET_PLAYER_STATE = 1,
    // What changed in a level, or all of it.
    M_PACKET_WORLD = 2,
    // A client asking the host for all of its level.
    M_PACKET_WORLD_REQUEST = 3,
    // A client saying who it is, first thing once connected.
    M_PACKET_HELLO = 4,
    // The host saying who is playing, and which of them the reader is.
    M_PACKET_ROSTER = 5,
    // The host saying which level it is playing, so that the others follow.
    M_PACKET_START = 6,
    // A client's profile, for the host to keep.
    M_PACKET_PROFILE = 7,
    // A profile the host hands back, for the client to go on from.
    M_PACKET_PROFILE_RESTORE = 8,
    // A player spending a large medipack on a dead one, to the host.
    M_PACKET_REVIVE_REQUEST = 9,
    // The host bringing a dead player back, at the side of who revived them.
    M_PACKET_REVIVE = 10,
    // A client reaching the level's exit, to the host.
    M_PACKET_AT_EXIT = 11,
    // The host saying how many of the players are at the exit, of how many.
    M_PACKET_EXIT_STATUS = 12,
    // The host's creatures: the moving ones every frame and unreliably, the
    // ones that changed, or all of them, reliably.
    M_PACKET_CREATURES = 13,
    // A client's shot landing on a creature, to the host.
    M_PACKET_HIT = 14,
    // The host passing on a blow a player's puppet took, to that player.
    M_PACKET_DAMAGE = 15,
    // A client's trigger waking a creature, to the host.
    M_PACKET_ACTIVATE = 16,
    // What the host's creatures set off this frame: effects and sounds.
    M_PACKET_CREATURE_EVENTS = 17,
} M_PACKET_TYPE;

// level, how many effects, how many sounds
#define M_CREATURE_EVENTS_HEADER_SIZE (M_HEADER_SIZE + 4 + 1 + 1)

// level, the host's frame, how many records
#define M_CREATURES_HEADER_SIZE (M_HEADER_SIZE + 4 + 4 + 1)
// Few enough records to keep a packet under a datagram.
#define M_CREATURES_PER_PACKET 24
// level, item, damage, flags
#define M_HIT_SIZE (M_HEADER_SIZE + 4 + 2 + 2 + 1)
// level, damage, flags
#define M_DAMAGE_SIZE (M_HEADER_SIZE + 4 + 2 + 1)
// level, item, whether forced
#define M_ACTIVATE_SIZE (M_HEADER_SIZE + 4 + 2 + 1)

// level
#define M_AT_EXIT_SIZE (M_HEADER_SIZE + 4)
// at the exit, of, and which players are there
#define M_EXIT_STATUS_SIZE (M_HEADER_SIZE + 1 + 1 + 1)
// How often a player waiting at the exit is reminded of it.
#define M_EXIT_REMINDER_FRAMES 150

// who, then where: room and pos
#define M_REVIVE_SIZE (M_HEADER_SIZE + 1 + 2 + 3 * 4)

// Why a connection ends, carried by the disconnect itself. A connection that
// is simply lost carries none, and a client tries the host again then.
typedef enum {
    M_END_DROPPED = 0,
    // The host left the game on purpose, so there is nothing to come back to.
    M_END_HOST_LEFT = 1,
    // The host would not take this player: the game is full, or runs another
    // version.
    M_END_REFUSED = 2,
} M_END_REASON;

// Every packet opens with the protocol version and its type.
#define M_HEADER_SIZE 2
#define M_PLAYER_STATE_SIZE (M_HEADER_SIZE + 1 + 4 + COOP_WIRE_STATE_SIZE)
// level, whether it is all of the level, whether the flips follow, item count
#define M_WORLD_HEADER_SIZE (M_HEADER_SIZE + 4 + 1 + 1 + 2)
#define M_WORLD_REQUEST_SIZE (M_HEADER_SIZE + 4)
// level, and how many times the host has loaded a level this session
#define M_START_SIZE (M_HEADER_SIZE + 4 + 4)
// id, name length, then the name
#define M_IDENTITY_MIN_SIZE (COOP_ID_SIZE + 1)
#define M_IDENTITY_MAX_SIZE (M_IDENTITY_MIN_SIZE + COOP_NAME_SIZE - 1)
#define M_HELLO_MAX_SIZE (M_HEADER_SIZE + M_IDENTITY_MAX_SIZE)
// the reader's own number and the player count, then per player its number,
// whether it is connected, and who it is
#define M_ROSTER_MAX_SIZE                                                      \
    (M_HEADER_SIZE + 1 + 1 + M_MAX_PLAYERS * (1 + 1 + M_IDENTITY_MAX_SIZE))

// Where a profile handed back puts Lara.
typedef enum {
    // Where she is, as at the start of the level.
    M_PLACE_NONE = 0,
    // Where the profile was taken.
    M_PLACE_PROFILE = 1,
    // At the spot the host names, at its own Lara's side.
    M_PLACE_SPOT = 2,
} M_PLACE;

// where, then the spot's room and pos, then the profile
#define M_RESTORE_HEADER_SIZE (M_HEADER_SIZE + 1 + 2 + 3 * 4)

// A profile as it travels, owned by whoever holds it.
typedef struct {
    uint8_t *data;
    size_t size;
} M_BLOB;

typedef struct {
    // Known to this session, connected or not: a player who drops keeps the
    // number to come back to.
    bool known;
    bool connected;
    uint8_t id[COOP_ID_SIZE];
    char name[COOP_NAME_SIZE];
    // On the host, the client's connection.
    ENetPeer *peer;
    bool has_state;
    int32_t level_num;
    COOP_PLAYER_STATE state;
    // On the host, the client's last profile, which outlasts its connection.
    M_BLOB profile;
    // On the host, whether the player reached the exit of the host's level.
    bool at_exit;
    // On the host, whether the player came back to the game with a profile
    // still held for them, and is still to be put back in it.
    bool returning;
} M_PLAYER;

// A profile from the host's save, waiting for its player to come back.
typedef struct {
    uint8_t id[COOP_ID_SIZE];
    char name[COOP_NAME_SIZE];
    M_BLOB profile;
    // Still to hand back, position and all.
    bool pending;
} M_SAVED_PROFILE;

typedef enum {
    M_ROLE_NONE,
    M_ROLE_HOST,
    M_ROLE_CLIENT,
} M_ROLE;

// Whether the command line has had its say, which it gets once a run.
static bool m_ArgsTaken = false;

static struct {
    M_ROLE role;
    bool running;
    ENetHost *host;
    // Where a client finds its host, kept to reconnect to.
    ENetAddress host_address;
    int32_t reconnect_wait;
    // A client's connection to its host.
    ENetPeer *host_peer;
    bool host_connected;

    M_PLAYER players[M_MAX_PLAYERS];
    // This game's number among them; on a client, unknown until the host
    // says.
    int32_t local_index;

    // On a client, whether this level has caught up with the host's since it
    // loaded. Until it has, what changes here is not worth sending.
    bool world_synced;
    int32_t world_request_wait;
    VECTOR *world_changes;
    VECTOR *world_taken;
    // On the host, the creatures to send this frame.
    VECTOR *creatures_moving;
    VECTOR *creatures_changed;
    VECTOR *creature_effects;
    VECTOR *creature_sounds;
    // On a client, the blows the host passed on, to take in play.
    int32_t damage_pending;
    bool damage_hit_status;
    bool damage_fatal;

    // On the host, the level last told to the others, or -1 while none is,
    // and how many levels it has loaded: the same level loaded again from a
    // save is a new start too.
    int32_t announced_level;
    int32_t load_count;
    // On a client, the last start it heard, and whether it is on its way to
    // the level that start named.
    int32_t heard_load_count;
    bool start_pending;
    int32_t pending_level;

    // On the host, the profiles its save brought back.
    M_SAVED_PROFILE saved[M_MAX_SAVED_PROFILES];
    // On a client, the profile last sent, to send only what changed, and one
    // the host handed back, to apply once the level has caught up.
    M_BLOB profile_sent;
    int32_t profile_wait;
    M_BLOB profile_restore;
    M_PLACE restore_place;
    int16_t restore_room;
    XYZ_32 restore_pos;
    // On a client, why the host ended the session, dealt with once the events
    // in hand are through.
    M_END_REASON ended;

    // Whether this player has been told it is waiting to be revived, and
    // whether each other player was alive when last seen, to say when one
    // goes down.
    bool told_waiting;
    bool was_alive[M_MAX_PLAYERS];
    // On a client, whether it told the host it reached the exit. On the host,
    // whether the last player to reach it lets the level end, and what it
    // last told the others of how many are there.
    bool told_at_exit;
    int32_t exit_count;
    int32_t exit_total;
    uint8_t exit_mask;
    int32_t exit_reminder_wait;
    // Whether this game has played a level in this session. Only then does
    // going back to the title leave it: the lobby is at the title too.
    bool played;
    // A revive the host granted this game, to apply in play.
    bool revive_pending;
    int32_t revived_by;
    XYZ_32 revive_pos;
    int16_t revive_room;
} m_Net = {
    .local_index = -1,
    .announced_level = -1,
    .heard_load_count = -1,
};

static bool M_IsHost(void)
{
    return m_Net.role == M_ROLE_HOST;
}

static bool M_IsValidIndex(const int32_t index)
{
    return index >= 0 && index < M_MAX_PLAYERS;
}

static bool M_IsPeerConnected(const ENetPeer *const peer)
{
    return peer != nullptr && peer->state == ENET_PEER_STATE_CONNECTED;
}

// The level being played, or -1 out of play. A menu's own level, such as the
// title's, can share a number with one in the game, so it never counts.
static int32_t M_GetLevelNum(void)
{
    if (!Coop_IsPlayableLevel()) {
        return -1;
    }
    return GF_GetCurrentLevel()->num;
}

static uint16_t M_GetPort(void)
{
    const int32_t port = Shell_GetArgs()->coop.port;
    return port > 0 ? (uint16_t)port : COOP_DEFAULT_PORT;
}

static void M_Send(
    ENetPeer *const peer, const uint8_t channel, const void *const data,
    const size_t size, const uint32_t flags)
{
    if (!M_IsPeerConnected(peer)) {
        return;
    }
    enet_peer_send(peer, channel, enet_packet_create(data, size, flags));
}

static void M_PutHeader(uint8_t **const cursor, const M_PACKET_TYPE type)
{
    Coop_Wire_PutU8(cursor, M_PROTOCOL_VERSION);
    Coop_Wire_PutU8(cursor, type);
}

static void M_PutIdentity(
    uint8_t **const cursor, const uint8_t id[COOP_ID_SIZE],
    const char *const name)
{
    memcpy(*cursor, id, COOP_ID_SIZE);
    *cursor += COOP_ID_SIZE;
    const size_t name_len = strlen(name);
    Coop_Wire_PutU8(cursor, (uint8_t)name_len);
    memcpy(*cursor, name, name_len);
    *cursor += name_len;
}

// Reads an identity no further than `end`, and returns false for one that
// runs past it or names nobody.
static bool M_GetIdentity(
    const uint8_t **const cursor, const uint8_t *const end,
    uint8_t id[COOP_ID_SIZE], char name[COOP_NAME_SIZE])
{
    if (end - *cursor < M_IDENTITY_MIN_SIZE) {
        return false;
    }
    memcpy(id, *cursor, COOP_ID_SIZE);
    *cursor += COOP_ID_SIZE;
    const size_t name_len = Coop_Wire_GetU8(cursor);
    if (name_len >= COOP_NAME_SIZE || (size_t)(end - *cursor) < name_len) {
        return false;
    }
    char raw[COOP_NAME_SIZE] = {};
    memcpy(raw, *cursor, name_len);
    *cursor += name_len;
    return Coop_Identity_CleanName(raw, name);
}

// Players --------------------------------------------------------------------

static M_PLAYER *M_GetPlayer(const int32_t index)
{
    return M_IsValidIndex(index) ? &m_Net.players[index] : nullptr;
}

static void M_ForgetPlayerState(const int32_t index)
{
    M_PLAYER *const player = M_GetPlayer(index);
    if (player == nullptr) {
        return;
    }
    player->has_state = false;
    Coop_Puppet_Remove(index);
}

static void M_SetUpLocalPlayer(void)
{
    const COOP_IDENTITY *const identity = Coop_Identity_Get();
    if (!M_IsHost()) {
        return;
    }
    m_Net.local_index = M_HOST_INDEX;
    M_PLAYER *const host = &m_Net.players[M_HOST_INDEX];
    host->known = true;
    host->connected = true;
    memcpy(host->id, identity->id, COOP_ID_SIZE);
    strcpy(host->name, identity->name);
}

// A returning id gets its old number back; a new one the first free number.
static int32_t M_PickIndexFor(const uint8_t id[COOP_ID_SIZE])
{
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        const M_PLAYER *const player = &m_Net.players[i];
        if (player->known && memcmp(player->id, id, COOP_ID_SIZE) == 0) {
            return player->connected ? -1 : i;
        }
    }
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        if (!m_Net.players[i].known) {
            return i;
        }
    }
    return -1;
}

static int32_t M_GetPeerIndex(const ENetPeer *const peer)
{
    const intptr_t index = (intptr_t)peer->data - 1;
    return M_IsValidIndex(index) ? (int32_t)index : -1;
}

// Roster ---------------------------------------------------------------------

static void M_SendRoster(ENetPeer *const peer, const int32_t reader_index)
{
    uint8_t data[M_ROSTER_MAX_SIZE];
    uint8_t *cursor = data;
    M_PutHeader(&cursor, M_PACKET_ROSTER);
    Coop_Wire_PutU8(&cursor, (uint8_t)reader_index);

    int32_t count = 0;
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        count += m_Net.players[i].known ? 1 : 0;
    }
    Coop_Wire_PutU8(&cursor, (uint8_t)count);
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        const M_PLAYER *const player = &m_Net.players[i];
        if (!player->known) {
            continue;
        }
        Coop_Wire_PutU8(&cursor, (uint8_t)i);
        Coop_Wire_PutU8(&cursor, player->connected ? 1 : 0);
        M_PutIdentity(&cursor, player->id, player->name);
    }
    M_Send(
        peer, M_CHANNEL_RELIABLE, data, cursor - data,
        ENET_PACKET_FLAG_RELIABLE);
}

static void M_BroadcastRoster(void)
{
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        const M_PLAYER *const player = &m_Net.players[i];
        if (i != m_Net.local_index && player->connected) {
            M_SendRoster(player->peer, i);
        }
    }
}

static void M_ReceiveRoster(const uint8_t *cursor, const uint8_t *const end)
{
    if (M_IsHost() || end - cursor < 2) {
        return;
    }
    const int32_t reader_index = Coop_Wire_GetU8(&cursor);
    const int32_t count = Coop_Wire_GetU8(&cursor);

    bool listed[M_MAX_PLAYERS] = {};
    for (int32_t i = 0; i < count; i++) {
        if (end - cursor < 2) {
            return;
        }
        const int32_t index = Coop_Wire_GetU8(&cursor);
        const bool connected = Coop_Wire_GetU8(&cursor) != 0;
        uint8_t id[COOP_ID_SIZE];
        char name[COOP_NAME_SIZE];
        if (!M_GetIdentity(&cursor, end, id, name) || !M_IsValidIndex(index)) {
            return;
        }
        M_PLAYER *const player = &m_Net.players[index];
        if (player->connected && !connected) {
            Console_Info("Coop: %s left", player->name);
        } else if (!player->connected && connected && index != reader_index) {
            Console_Info("Coop: %s is playing", name);
        }
        player->known = true;
        player->connected = connected;
        memcpy(player->id, id, COOP_ID_SIZE);
        strcpy(player->name, name);
        listed[index] = true;
        if (!connected) {
            M_ForgetPlayerState(index);
        }
    }
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        if (!listed[i] && m_Net.players[i].known) {
            m_Net.players[i].known = false;
            m_Net.players[i].connected = false;
            M_ForgetPlayerState(i);
        }
    }
    m_Net.local_index = M_IsValidIndex(reader_index) ? reader_index : -1;
}

// Start ----------------------------------------------------------------------

static void M_SendStart(ENetPeer *const peer)
{
    uint8_t data[M_START_SIZE];
    uint8_t *cursor = data;
    M_PutHeader(&cursor, M_PACKET_START);
    Coop_Wire_PutS32(&cursor, m_Net.announced_level);
    Coop_Wire_PutS32(&cursor, m_Net.load_count);
    M_Send(
        peer, M_CHANNEL_RELIABLE, data, sizeof(data),
        ENET_PACKET_FLAG_RELIABLE);
}

static void M_ForgetExit(void);

// Tells the others whenever the host comes into a level, a level loaded again
// from a save included, so that they go where it went. Without `force`, only
// a level other than the last one told counts.
static void M_AnnounceLevel(const int32_t level_num, const bool force)
{
    if (!force && m_Net.announced_level == level_num) {
        return;
    }
    m_Net.announced_level = level_num;
    m_Net.load_count++;
    // A new start, so nobody is at its exit yet.
    M_ForgetExit();
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        if (i != M_HOST_INDEX && m_Net.players[i].connected) {
            M_SendStart(m_Net.players[i].peer);
        }
    }
}

static void M_ReceiveStart(const ENetPacket *const packet)
{
    if (M_IsHost() || packet->dataLength != M_START_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const int32_t level_num = Coop_Wire_GetS32(&cursor);
    const int32_t load_count = Coop_Wire_GetS32(&cursor);
    if (load_count == m_Net.heard_load_count) {
        return;
    }
    m_Net.heard_load_count = load_count;
    // Already on the way there, as the host announces a level both as its
    // sequence starts and as play begins.
    if (m_Net.start_pending && m_Net.pending_level == level_num) {
        return;
    }

    // Already there: the host loaded it afresh, so only the world needs to
    // catch up with the host's again. A dead Lara cannot pick up from there,
    // so she loads it afresh too, and the host's save hands her back.
    const ITEM *const lara_item = Lara_GetItem();
    const bool is_alive = lara_item != nullptr && lara_item->hit_points > 0;
    if (level_num == M_GetLevelNum() && is_alive) {
        m_Net.world_synced = false;
        m_Net.world_request_wait = 0;
        return;
    }

    if (GF_GetLevel(GFLT_MAIN, level_num) == nullptr) {
        LOG_WARNING("Coop: the host is in level %d, which this game lacks",
            level_num);
        return;
    }
    // Goes the way the level select does, with Lara carrying what she would
    // on reaching the level. A screen open in front waits to close first.
    SG_Manager_UnbindSlot();
    GF_OverrideCommand(
        (GF_COMMAND) { .action = GF_SELECT_GAME, .param = level_num },
        !UI_Takeover_IsAnyHeld());
    m_Net.start_pending = true;
    m_Net.pending_level = level_num;
    Console_Info("Coop: following the host");
}

// Hello ----------------------------------------------------------------------

static void M_SendHello(void)
{
    const COOP_IDENTITY *const identity = Coop_Identity_Get();
    uint8_t data[M_HELLO_MAX_SIZE];
    uint8_t *cursor = data;
    M_PutHeader(&cursor, M_PACKET_HELLO);
    M_PutIdentity(&cursor, identity->id, identity->name);
    M_Send(
        m_Net.host_peer, M_CHANNEL_RELIABLE, data, cursor - data,
        ENET_PACKET_FLAG_RELIABLE);
}

static void M_ReceiveHello(
    ENetPeer *const peer, const uint8_t *cursor, const uint8_t *const end)
{
    if (!M_IsHost() || M_GetPeerIndex(peer) >= 0) {
        return;
    }
    uint8_t id[COOP_ID_SIZE];
    char name[COOP_NAME_SIZE];
    if (!M_GetIdentity(&cursor, end, id, name)) {
        enet_peer_disconnect(peer, M_END_REFUSED);
        return;
    }

    const int32_t index = M_PickIndexFor(id);
    if (index < 0) {
        LOG_WARNING("Coop: turned away %s, the game is full", name);
        enet_peer_disconnect(peer, M_END_REFUSED);
        return;
    }

    M_PLAYER *const player = &m_Net.players[index];
    // A player coming back keeps the profile the host held for them.
    const M_BLOB profile = player->profile;
    *player = (M_PLAYER) {
        .known = true,
        .connected = true,
        .peer = peer,
        .profile = profile,
        .returning = profile.data != nullptr,
    };
    memcpy(player->id, id, COOP_ID_SIZE);
    strcpy(player->name, name);
    peer->data = (void *)(intptr_t)(index + 1);
    Console_Info("Coop: %s joined as player %d", name, index + 1);
    M_BroadcastRoster();
    // A player arriving mid-game goes straight to the host's level.
    if (m_Net.announced_level >= 0) {
        M_SendStart(peer);
    }
}

// Player states ---------------------------------------------------------------

static void M_SendLocalState(const int32_t level_num)
{
    if (!M_IsValidIndex(m_Net.local_index)) {
        return;
    }

    uint8_t data[M_PLAYER_STATE_SIZE];
    uint8_t *cursor = data;
    const COOP_PLAYER_STATE state = Coop_Puppet_CaptureLara();
    M_PutHeader(&cursor, M_PACKET_PLAYER_STATE);
    Coop_Wire_PutU8(&cursor, (uint8_t)m_Net.local_index);
    Coop_Wire_PutS32(&cursor, level_num);
    Coop_Wire_PutState(&cursor, &state);
    ASSERT(cursor == data + sizeof(data));

    // Unreliable and sequenced: a lost state is replaced by the next frame's,
    // and one that arrives late is dropped rather than played out of order.
    if (!M_IsHost()) {
        M_Send(m_Net.host_peer, M_CHANNEL_STATE, data, sizeof(data), 0);
        return;
    }
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        if (i != M_HOST_INDEX && m_Net.players[i].connected) {
            M_Send(
                m_Net.players[i].peer, M_CHANNEL_STATE, data, sizeof(data),
                0);
        }
    }
}

static void M_ReceivePlayerState(
    ENetPeer *const peer, const ENetPacket *const packet)
{
    if (packet->dataLength != M_PLAYER_STATE_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const int32_t index = Coop_Wire_GetU8(&cursor);
    // The host takes a state only from the player it belongs to, and passes
    // it on to everyone else; a client takes the host's word on who sent it.
    if (M_IsHost() ? index != M_GetPeerIndex(peer)
                   : (!M_IsValidIndex(index) || index == m_Net.local_index)) {
        return;
    }

    M_PLAYER *const player = &m_Net.players[index];
    player->level_num = Coop_Wire_GetS32(&cursor);
    Coop_Wire_GetState(&cursor, &player->state);
    player->has_state = true;

    if (M_IsHost()) {
        for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
            if (i != M_HOST_INDEX && i != index
                && m_Net.players[i].connected) {
                M_Send(
                    m_Net.players[i].peer, M_CHANNEL_STATE, packet->data,
                    packet->dataLength, 0);
            }
        }
    }
}

// World -----------------------------------------------------------------------

static void M_SendWorld(
    ENetPeer *const peer, const int32_t level_num, const VECTOR *const items,
    const COOP_FLIP_RECORD *const flip, const bool is_all)
{
    if (!M_IsPeerConnected(peer) || (items->count == 0 && flip == nullptr)) {
        return;
    }

    const size_t size = M_WORLD_HEADER_SIZE
        + (flip != nullptr ? COOP_WIRE_FLIP_SIZE : 0)
        + items->count * COOP_WIRE_ITEM_RECORD_SIZE;
    ENetPacket *const packet =
        enet_packet_create(nullptr, size, ENET_PACKET_FLAG_RELIABLE);
    uint8_t *cursor = packet->data;
    M_PutHeader(&cursor, M_PACKET_WORLD);
    Coop_Wire_PutS32(&cursor, level_num);
    Coop_Wire_PutU8(&cursor, is_all ? 1 : 0);
    Coop_Wire_PutU8(&cursor, flip != nullptr ? 1 : 0);
    Coop_Wire_PutS16(&cursor, (int16_t)items->count);
    if (flip != nullptr) {
        Coop_Wire_PutFlip(&cursor, flip);
    }
    for (int32_t i = 0; i < items->count; i++) {
        Coop_Wire_PutItemRecord(&cursor, Vector_Get(items, i));
    }
    ASSERT(cursor == packet->data + size);
    enet_peer_send(peer, M_CHANNEL_RELIABLE, packet);
}

// The host's own changes, or a client's it took in, to every other client in
// the level.
static void M_HostSendWorld(
    const int32_t level_num, const int32_t except_index,
    const VECTOR *const items, const COOP_FLIP_RECORD *const flip)
{
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        const M_PLAYER *const player = &m_Net.players[i];
        if (i == M_HOST_INDEX || i == except_index || !player->connected
            || !player->has_state || player->level_num != level_num) {
            continue;
        }
        M_SendWorld(player->peer, level_num, items, flip, false);
    }
}

static void M_SendWorldRequest(const int32_t level_num)
{
    uint8_t data[M_WORLD_REQUEST_SIZE];
    uint8_t *cursor = data;
    M_PutHeader(&cursor, M_PACKET_WORLD_REQUEST);
    Coop_Wire_PutS32(&cursor, level_num);
    M_Send(
        m_Net.host_peer, M_CHANNEL_RELIABLE, data, sizeof(data),
        ENET_PACKET_FLAG_RELIABLE);
}

static void M_ReceiveWorld(
    ENetPeer *const peer, const ENetPacket *const packet)
{
    if (packet->dataLength < M_WORLD_HEADER_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const int32_t level_num = Coop_Wire_GetS32(&cursor);
    const bool is_all = Coop_Wire_GetU8(&cursor) != 0;
    const bool has_flip = Coop_Wire_GetU8(&cursor) != 0;
    const int32_t count = (uint16_t)Coop_Wire_GetS16(&cursor);
    const size_t expected = M_WORLD_HEADER_SIZE
        + (has_flip ? COOP_WIRE_FLIP_SIZE : 0)
        + count * COOP_WIRE_ITEM_RECORD_SIZE;
    // A level this game is not in has nothing to take from it.
    if (packet->dataLength != expected || level_num != M_GetLevelNum()) {
        return;
    }

    // The host's word is final. A client's stands unless the host changed the
    // same thing itself a moment ago, and what the host takes it passes on.
    const bool authoritative = !M_IsHost();
    COOP_FLIP_RECORD flip;
    bool flip_taken = false;
    if (has_flip) {
        Coop_Wire_GetFlip(&cursor, &flip);
        flip_taken = Coop_World_ApplyFlip(&flip, authoritative);
    }
    Vector_Clear(m_Net.world_taken);
    for (int32_t i = 0; i < count; i++) {
        COOP_ITEM_RECORD record;
        Coop_Wire_GetItemRecord(&cursor, &record);
        if (Coop_World_ApplyItem(&record, authoritative)) {
            Vector_Add(m_Net.world_taken, &record);
        }
    }

    if (M_IsHost()) {
        M_HostSendWorld(
            level_num, M_GetPeerIndex(peer), m_Net.world_taken,
            flip_taken ? &flip : nullptr);
    } else if (is_all) {
        m_Net.world_synced = true;
        Console_Info("Coop: caught up with the host's level");
    }
}

// Creatures -------------------------------------------------------------------

static void M_SendCreatures(
    ENetPeer *const peer, const int32_t level_num, const VECTOR *const records,
    const bool reliable)
{
    if (!M_IsPeerConnected(peer)) {
        return;
    }
    for (int32_t first = 0; first < records->count;
         first += M_CREATURES_PER_PACKET) {
        const int32_t count =
            MIN(records->count - first, M_CREATURES_PER_PACKET);
        const size_t size =
            M_CREATURES_HEADER_SIZE + count * COOP_WIRE_CREATURE_RECORD_SIZE;
        ENetPacket *const packet = enet_packet_create(
            nullptr, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        uint8_t *cursor = packet->data;
        M_PutHeader(&cursor, M_PACKET_CREATURES);
        Coop_Wire_PutS32(&cursor, level_num);
        Coop_Wire_PutS32(&cursor, Coop_Creatures_GetFrame());
        Coop_Wire_PutU8(&cursor, (uint8_t)count);
        for (int32_t i = 0; i < count; i++) {
            Coop_Wire_PutCreatureRecord(
                &cursor, Vector_Get(records, first + i));
        }
        ASSERT(cursor == packet->data + size);
        enet_peer_send(
            peer, reliable ? M_CHANNEL_RELIABLE : M_CHANNEL_STATE, packet);
    }
}

static void M_SendAllCreatures(ENetPeer *const peer, const int32_t level_num)
{
    Coop_Creatures_GetAll(m_Net.creatures_changed);
    M_SendCreatures(peer, level_num, m_Net.creatures_changed, true);
    Vector_Clear(m_Net.creatures_changed);
}

static void M_ReceiveCreatures(const ENetPacket *const packet)
{
    if (M_IsHost() || packet->dataLength < M_CREATURES_HEADER_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const int32_t level_num = Coop_Wire_GetS32(&cursor);
    const int32_t frame = Coop_Wire_GetS32(&cursor);
    const int32_t count = Coop_Wire_GetU8(&cursor);
    const size_t expected =
        M_CREATURES_HEADER_SIZE + count * COOP_WIRE_CREATURE_RECORD_SIZE;
    if (packet->dataLength != expected || level_num != M_GetLevelNum()) {
        return;
    }
    for (int32_t i = 0; i < count; i++) {
        COOP_CREATURE_RECORD record;
        Coop_Wire_GetCreatureRecord(&cursor, &record);
        Coop_Creatures_Receive(&record, frame);
    }
}

static void M_SendCreatureEvents(
    ENetPeer *const peer, const int32_t level_num)
{
    const VECTOR *const effects = m_Net.creature_effects;
    const VECTOR *const sounds = m_Net.creature_sounds;
    if (!M_IsPeerConnected(peer)
        || (effects->count == 0 && sounds->count == 0)) {
        return;
    }
    const size_t size = M_CREATURE_EVENTS_HEADER_SIZE
        + effects->count * COOP_WIRE_EFFECT_RECORD_SIZE
        + sounds->count * COOP_WIRE_SOUND_RECORD_SIZE;
    ENetPacket *const packet =
        enet_packet_create(nullptr, size, ENET_PACKET_FLAG_RELIABLE);
    uint8_t *cursor = packet->data;
    M_PutHeader(&cursor, M_PACKET_CREATURE_EVENTS);
    Coop_Wire_PutS32(&cursor, level_num);
    Coop_Wire_PutU8(&cursor, (uint8_t)effects->count);
    Coop_Wire_PutU8(&cursor, (uint8_t)sounds->count);
    for (int32_t i = 0; i < effects->count; i++) {
        Coop_Wire_PutEffectRecord(&cursor, Vector_Get(effects, i));
    }
    for (int32_t i = 0; i < sounds->count; i++) {
        Coop_Wire_PutSoundRecord(&cursor, Vector_Get(sounds, i));
    }
    ASSERT(cursor == packet->data + size);
    enet_peer_send(peer, M_CHANNEL_RELIABLE, packet);
}

static void M_ReceiveCreatureEvents(const ENetPacket *const packet)
{
    if (M_IsHost() || packet->dataLength < M_CREATURE_EVENTS_HEADER_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const int32_t level_num = Coop_Wire_GetS32(&cursor);
    const int32_t effect_count = Coop_Wire_GetU8(&cursor);
    const int32_t sound_count = Coop_Wire_GetU8(&cursor);
    const size_t expected = M_CREATURE_EVENTS_HEADER_SIZE
        + effect_count * COOP_WIRE_EFFECT_RECORD_SIZE
        + sound_count * COOP_WIRE_SOUND_RECORD_SIZE;
    // Only into a level in play: a menu in front has nothing to show them.
    if (packet->dataLength != expected || level_num != M_GetLevelNum()
        || !Game_IsPlaying()) {
        return;
    }
    for (int32_t i = 0; i < effect_count; i++) {
        COOP_EFFECT_RECORD record;
        Coop_Wire_GetEffectRecord(&cursor, &record);
        Coop_Creatures_PlayEffect(&record);
    }
    for (int32_t i = 0; i < sound_count; i++) {
        COOP_SOUND_RECORD record;
        Coop_Wire_GetSoundRecord(&cursor, &record);
        Coop_Creatures_PlaySound(&record);
    }
}

// The host runs the creatures and tells the others; a client puts them where
// it is told.
static void M_SyncCreatures(const int32_t level_num)
{
    if (!M_IsHost()) {
        Coop_Creatures_ApplyReceived();
        return;
    }
    Coop_Creatures_Take(m_Net.creatures_moving, m_Net.creatures_changed);
    Coop_Creatures_TakeEvents(m_Net.creature_effects, m_Net.creature_sounds);
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        const M_PLAYER *const player = &m_Net.players[i];
        if (i == M_HOST_INDEX || !player->connected || !player->has_state
            || player->level_num != level_num) {
            continue;
        }
        M_SendCreatures(player->peer, level_num, m_Net.creatures_changed, true);
        M_SendCreatures(player->peer, level_num, m_Net.creatures_moving, false);
        M_SendCreatureEvents(player->peer, level_num);
    }
}

bool Coop_Net_SendHit(
    const int16_t item_num, const int16_t damage, const uint8_t flags)
{
    if (M_IsHost() || !m_Net.host_connected) {
        return false;
    }
    uint8_t data[M_HIT_SIZE];
    uint8_t *cursor = data;
    M_PutHeader(&cursor, M_PACKET_HIT);
    Coop_Wire_PutS32(&cursor, M_GetLevelNum());
    Coop_Wire_PutS16(&cursor, item_num);
    Coop_Wire_PutS16(&cursor, damage);
    Coop_Wire_PutU8(&cursor, flags);
    ASSERT(cursor == data + sizeof(data));
    M_Send(
        m_Net.host_peer, M_CHANNEL_RELIABLE, data, sizeof(data),
        ENET_PACKET_FLAG_RELIABLE);
    return true;
}

static void M_ReceiveHit(const ENetPacket *const packet)
{
    if (!M_IsHost() || packet->dataLength != M_HIT_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const int32_t level_num = Coop_Wire_GetS32(&cursor);
    const int16_t item_num = Coop_Wire_GetS16(&cursor);
    const int16_t damage = Coop_Wire_GetS16(&cursor);
    const uint8_t flags = Coop_Wire_GetU8(&cursor);
    if (level_num != M_GetLevelNum() || !Coop_Creatures_IsShared(item_num)) {
        return;
    }
    ITEM *const item = Item_Get(item_num);
    if (item->is_destroyed) {
        return;
    }
    // Credited to Lara, so the kill counts and the creature knows it was
    // hurt by a player.
    Item_TakeDamage(
        item, damage, flags & (IDF_NO_HIT_STATUS | IDF_NO_KILL_STATS),
        Lara_GetItem());
}

void Coop_Net_SendDamage(
    const int32_t index, const int16_t damage, const uint8_t flags)
{
    const M_PLAYER *const player = M_GetPlayer(index);
    if (!M_IsHost() || index == M_HOST_INDEX || player == nullptr
        || !player->connected) {
        return;
    }
    uint8_t data[M_DAMAGE_SIZE];
    uint8_t *cursor = data;
    M_PutHeader(&cursor, M_PACKET_DAMAGE);
    Coop_Wire_PutS32(&cursor, M_GetLevelNum());
    Coop_Wire_PutS16(&cursor, damage);
    Coop_Wire_PutU8(&cursor, flags);
    ASSERT(cursor == data + sizeof(data));
    M_Send(
        player->peer, M_CHANNEL_RELIABLE, data, sizeof(data),
        ENET_PACKET_FLAG_RELIABLE);
}

static void M_ReceiveDamage(const ENetPacket *const packet)
{
    if (M_IsHost() || packet->dataLength != M_DAMAGE_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const int32_t level_num = Coop_Wire_GetS32(&cursor);
    const int16_t damage = Coop_Wire_GetS16(&cursor);
    const uint8_t flags = Coop_Wire_GetU8(&cursor);
    if (level_num != M_GetLevelNum()) {
        return;
    }
    if ((flags & IDF_FATAL) != 0) {
        m_Net.damage_fatal = true;
        return;
    }
    if (damage > 0) {
        m_Net.damage_pending += damage;
    }
    if ((flags & IDF_NO_HIT_STATUS) == 0) {
        m_Net.damage_hit_status = true;
    }
}

// What the host said Lara suffered, taken in play rather than in a menu.
static void M_ApplyDamage(void)
{
    const int32_t damage = m_Net.damage_pending;
    const bool hit_status = m_Net.damage_hit_status;
    const bool fatal = m_Net.damage_fatal;
    m_Net.damage_pending = 0;
    m_Net.damage_hit_status = false;
    m_Net.damage_fatal = false;

    ITEM *const lara_item = Lara_GetItem();
    if (lara_item == nullptr || lara_item->hit_points <= 0
        || g_Config.debug.enable_invulnerability) {
        return;
    }
    if (fatal) {
        Item_TakeFatalDamage(lara_item, nullptr);
    } else if (damage > 0) {
        Item_TakeDamage(
            lara_item, (int16_t)MIN(damage, INT16_MAX),
            hit_status ? IDF_NONE : IDF_NO_HIT_STATUS, nullptr);
    }
}

bool Coop_Net_SendActivate(const int16_t item_num, const bool force)
{
    if (M_IsHost() || !m_Net.host_connected) {
        return false;
    }
    uint8_t data[M_ACTIVATE_SIZE];
    uint8_t *cursor = data;
    M_PutHeader(&cursor, M_PACKET_ACTIVATE);
    Coop_Wire_PutS32(&cursor, M_GetLevelNum());
    Coop_Wire_PutS16(&cursor, item_num);
    Coop_Wire_PutU8(&cursor, force ? 1 : 0);
    ASSERT(cursor == data + sizeof(data));
    M_Send(
        m_Net.host_peer, M_CHANNEL_RELIABLE, data, sizeof(data),
        ENET_PACKET_FLAG_RELIABLE);
    return true;
}

static void M_ReceiveActivate(const ENetPacket *const packet)
{
    if (!M_IsHost() || packet->dataLength != M_ACTIVATE_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const int32_t level_num = Coop_Wire_GetS32(&cursor);
    const int16_t item_num = Coop_Wire_GetS16(&cursor);
    const bool force = Coop_Wire_GetU8(&cursor) != 0;
    if (level_num != M_GetLevelNum() || !Coop_Creatures_IsShared(item_num)
        || Item_Get(item_num)->is_destroyed) {
        return;
    }
    Item_Activate(item_num, force);
}

static void M_HandBackProfile(int32_t index);

static void M_ReceiveWorldRequest(
    ENetPeer *const peer, const ENetPacket *const packet)
{
    if (packet->dataLength != M_WORLD_REQUEST_SIZE || !M_IsHost()) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const int32_t level_num = Coop_Wire_GetS32(&cursor);
    if (level_num != M_GetLevelNum()) {
        return;
    }
    COOP_FLIP_RECORD flip;
    Coop_World_GetAll(m_Net.world_taken, &flip);
    M_SendWorld(peer, level_num, m_Net.world_taken, &flip, true);
    Vector_Clear(m_Net.world_taken);
    M_SendAllCreatures(peer, level_num);
    // Behind the world on the same channel, so it lands on a level that has
    // caught up.
    M_HandBackProfile(M_GetPeerIndex(peer));
}

static void M_SyncWorld(const int32_t level_num)
{
    // Taken every frame, connected or not, so that what is sent later is only
    // what changed since then.
    COOP_FLIP_RECORD flip;
    const bool flip_changed =
        Coop_World_TakeChanges(m_Net.world_changes, &flip);
    const COOP_FLIP_RECORD *const flip_sent = flip_changed ? &flip : nullptr;

    if (M_IsHost()) {
        M_HostSendWorld(level_num, -1, m_Net.world_changes, flip_sent);
        return;
    }

    const M_PLAYER *const host = &m_Net.players[M_HOST_INDEX];
    if (!m_Net.host_connected || !host->has_state
        || host->level_num != level_num) {
        return;
    }

    // A client catches up with the host's level before it has anything of its
    // own worth telling.
    if (!m_Net.world_synced) {
        if (m_Net.world_request_wait-- <= 0) {
            M_SendWorldRequest(level_num);
            m_Net.world_request_wait = M_WORLD_REQUEST_FRAMES;
        }
        return;
    }
    M_SendWorld(
        m_Net.host_peer, level_num, m_Net.world_changes, flip_sent, false);
}

// Profiles --------------------------------------------------------------------

static void M_FreeBlob(M_BLOB *const blob)
{
    Memory_FreePointer(&blob->data);
    blob->size = 0;
}

static void M_SetBlob(
    M_BLOB *const blob, const void *const data, const size_t size)
{
    M_FreeBlob(blob);
    if (data == nullptr || size == 0) {
        return;
    }
    blob->data = Memory_Alloc(size);
    memcpy(blob->data, data, size);
    blob->size = size;
}

static bool M_IsSameBlob(
    const M_BLOB *const blob, const void *const data, const size_t size)
{
    return blob->data != nullptr && blob->size == size
        && memcmp(blob->data, data, size) == 0;
}

// A client's profile to the host. Without `force`, only when it changed since
// the last one sent.
static void M_SendProfile(const bool force)
{
    if (M_IsHost() || !M_IsPeerConnected(m_Net.host_peer)
        || !M_IsValidIndex(m_Net.local_index)) {
        return;
    }
    void *data = nullptr;
    size_t size = 0;
    if (!Coop_Profile_Capture(&data, &size)) {
        return;
    }
    if (force || !M_IsSameBlob(&m_Net.profile_sent, data, size)) {
        ENetPacket *const packet = enet_packet_create(
            nullptr, M_HEADER_SIZE + size, ENET_PACKET_FLAG_RELIABLE);
        uint8_t *cursor = packet->data;
        M_PutHeader(&cursor, M_PACKET_PROFILE);
        memcpy(cursor, data, size);
        enet_peer_send(m_Net.host_peer, M_CHANNEL_RELIABLE, packet);
        M_SetBlob(&m_Net.profile_sent, data, size);
    }
    Memory_Free(data);
}

static void M_ReceiveProfile(
    ENetPeer *const peer, const ENetPacket *const packet)
{
    M_PLAYER *const player = M_GetPlayer(M_GetPeerIndex(peer));
    if (!M_IsHost() || player == nullptr
        || packet->dataLength <= M_HEADER_SIZE
        || packet->dataLength > M_HEADER_SIZE + COOP_PROFILE_MAX_SIZE) {
        return;
    }
    M_SetBlob(
        &player->profile, packet->data + M_HEADER_SIZE,
        packet->dataLength - M_HEADER_SIZE);
}

static void M_SendRestoreAt(
    ENetPeer *const peer, const M_BLOB *const blob, const M_PLACE place,
    const int16_t room_num, const XYZ_32 pos)
{
    if (!M_IsPeerConnected(peer) || blob->data == nullptr) {
        return;
    }
    ENetPacket *const packet = enet_packet_create(
        nullptr, M_RESTORE_HEADER_SIZE + blob->size,
        ENET_PACKET_FLAG_RELIABLE);
    uint8_t *cursor = packet->data;
    M_PutHeader(&cursor, M_PACKET_PROFILE_RESTORE);
    Coop_Wire_PutU8(&cursor, (uint8_t)place);
    Coop_Wire_PutS16(&cursor, room_num);
    Coop_Wire_PutS32(&cursor, pos.x);
    Coop_Wire_PutS32(&cursor, pos.y);
    Coop_Wire_PutS32(&cursor, pos.z);
    memcpy(cursor, blob->data, blob->size);
    enet_peer_send(peer, M_CHANNEL_RELIABLE, packet);
}

static void M_SendRestore(
    ENetPeer *const peer, const M_BLOB *const blob, const bool with_position)
{
    M_SendRestoreAt(
        peer, blob, with_position ? M_PLACE_PROFILE : M_PLACE_NONE, NO_ROOM,
        (XYZ_32) {});
}

static bool M_IsHostAlive(void);

// A player back in the game picks up where they left when that was in this
// level, dead as they were if they left dead. Back in a level they never
// played, they come in at the host's side, or at its start while the host is
// down too.
static void M_SendReturningRestore(
    ENetPeer *const peer, const M_BLOB *const profile)
{
    const int32_t level_num =
        Coop_Profile_GetLevelNum(profile->data, profile->size);
    if (level_num == M_GetLevelNum()) {
        M_SendRestore(peer, profile, true);
    } else if (M_IsHostAlive()) {
        const ITEM *const lara_item = Lara_GetItem();
        M_SendRestoreAt(
            peer, profile, M_PLACE_SPOT, lara_item->room_num, lara_item->pos);
    } else {
        M_SendRestore(peer, profile, false);
    }
}

static M_SAVED_PROFILE *M_FindSaved(const uint8_t id[COOP_ID_SIZE])
{
    for (int32_t i = 0; i < M_MAX_SAVED_PROFILES; i++) {
        M_SAVED_PROFILE *const saved = &m_Net.saved[i];
        if (saved->profile.data != nullptr
            && memcmp(saved->id, id, COOP_ID_SIZE) == 0) {
            return saved;
        }
    }
    return nullptr;
}

// What a client goes on from as it catches up with the host's level: where
// the host's save left it; else what it carried a moment ago, as it follows
// the host to another level; else, for someone the host never saw, a copy of
// the host's own kit, at the host's side.
static void M_HandBackProfile(const int32_t index)
{
    M_PLAYER *const player = M_GetPlayer(index);
    if (player == nullptr || !player->connected) {
        return;
    }

    M_SAVED_PROFILE *const saved = M_FindSaved(player->id);
    if (saved != nullptr && saved->pending) {
        saved->pending = false;
        M_SetBlob(&player->profile, saved->profile.data, saved->profile.size);
        M_SendRestore(player->peer, &saved->profile, true);
        return;
    }
    if (player->profile.data != nullptr) {
        if (player->returning) {
            player->returning = false;
            M_SendReturningRestore(player->peer, &player->profile);
        } else {
            M_SendRestore(player->peer, &player->profile, false);
        }
        return;
    }

    void *data = nullptr;
    size_t size = 0;
    if (Coop_Profile_Capture(&data, &size)) {
        const M_BLOB host_kit = { .data = data, .size = size };
        M_SendRestore(player->peer, &host_kit, true);
        Memory_Free(data);
    }
}

static void M_ReceiveRestore(const ENetPacket *const packet)
{
    if (M_IsHost() || packet->dataLength <= M_RESTORE_HEADER_SIZE
        || packet->dataLength > M_RESTORE_HEADER_SIZE + COOP_PROFILE_MAX_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const uint8_t place = Coop_Wire_GetU8(&cursor);
    m_Net.restore_place = place <= M_PLACE_SPOT ? place : M_PLACE_NONE;
    m_Net.restore_room = Coop_Wire_GetS16(&cursor);
    m_Net.restore_pos.x = Coop_Wire_GetS32(&cursor);
    m_Net.restore_pos.y = Coop_Wire_GetS32(&cursor);
    m_Net.restore_pos.z = Coop_Wire_GetS32(&cursor);
    M_SetBlob(
        &m_Net.profile_restore, cursor,
        packet->dataLength - M_RESTORE_HEADER_SIZE);
}

// Applies what the host handed back once the level has caught up with its
// own, and from then on keeps the host told of what changes.
static void M_SyncProfile(void)
{
    if (M_IsHost() || !m_Net.world_synced) {
        return;
    }
    if (m_Net.profile_restore.data != nullptr) {
        const M_BLOB *const restore = &m_Net.profile_restore;
        const bool in_place = m_Net.restore_place == M_PLACE_PROFILE;
        const bool was_dead = in_place
            && Coop_Profile_GetLevelNum(restore->data, restore->size)
                == M_GetLevelNum()
            && Coop_Profile_GetHitPoints(restore->data, restore->size) <= 0;
        Coop_Profile_Apply(
            m_Net.profile_restore.data, m_Net.profile_restore.size, in_place);
        M_FreeBlob(&m_Net.profile_restore);
        // Back where she fell, she is still down, waiting to be revived.
        if (was_dead) {
            Lara_Kill();
        }
        if (m_Net.restore_place == M_PLACE_SPOT
            && m_Net.restore_room >= 0 && m_Net.restore_room < Room_GetCount()
            && Lara_Cheat_Teleport(m_Net.restore_pos, m_Net.restore_room)) {
            const M_PLAYER *const host = M_GetPlayer(M_HOST_INDEX);
            Console_Info("Coop: back in at %s's side", host->name);
        } else {
            Console_Info("Coop: back where the host's game left you");
        }
        m_Net.profile_wait = 0;
    }
    if (m_Net.profile_wait-- <= 0) {
        M_SendProfile(false);
        m_Net.profile_wait = M_PROFILE_FRAMES;
    }
}

static char *M_ToHex(const uint8_t *const data, const size_t size)
{
    static const char digits[] = "0123456789abcdef";
    char *const hex = Memory_Alloc(size * 2 + 1);
    for (size_t i = 0; i < size; i++) {
        hex[i * 2] = digits[data[i] >> 4];
        hex[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    hex[size * 2] = '\0';
    return hex;
}

static int32_t M_HexDigit(const char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// Returns false, filling nothing, for text that is not whole bytes of hex.
static bool M_FromHex(
    const char *const hex, uint8_t *const out, const size_t out_size)
{
    const size_t len = strlen(hex);
    if (len != out_size * 2) {
        return false;
    }
    for (size_t i = 0; i < out_size; i++) {
        const int32_t hi = M_HexDigit(hex[i * 2]);
        const int32_t lo = M_HexDigit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void M_WriteProfile(
    JSON_WRITE_IO *const io, const uint8_t id[COOP_ID_SIZE],
    const char *const name, const M_BLOB *const profile)
{
    char *const id_hex = M_ToHex(id, COOP_ID_SIZE);
    char *const profile_hex = M_ToHex(profile->data, profile->size);
    JSONW_PUSH_OBJECT(io);
    JSONW_WRITE(io, "id", id_hex);
    JSONW_WRITE(io, "name", name);
    JSONW_WRITE(io, "profile", profile_hex);
    JSONW_POP_AND_APPEND(io);
    Memory_Free(profile_hex);
    Memory_Free(id_hex);
}

static void M_ForgetSaved(void)
{
    for (int32_t i = 0; i < M_MAX_SAVED_PROFILES; i++) {
        M_FreeBlob(&m_Net.saved[i].profile);
        m_Net.saved[i] = (M_SAVED_PROFILE) {};
    }
}

void Coop_Net_DumpProfiles(JSON_WRITE_IO *const io)
{
    if (!M_IsHost()) {
        return;
    }
    JSONW_PUSH_OBJECT(io);
    JSONW_PUSH_ARRAY(io);
    int32_t written = 0;
    for (int32_t i = 0; i < M_MAX_PLAYERS && written < M_MAX_SAVED_PROFILES;
         i++) {
        const M_PLAYER *const player = &m_Net.players[i];
        if (i != M_HOST_INDEX && player->known
            && player->profile.data != nullptr) {
            M_WriteProfile(io, player->id, player->name, &player->profile);
            written++;
        }
    }
    // Those who were in the save and have not come back keep their place.
    for (int32_t i = 0; i < M_MAX_SAVED_PROFILES && written < M_MAX_SAVED_PROFILES;
         i++) {
        const M_SAVED_PROFILE *const saved = &m_Net.saved[i];
        if (saved->profile.data == nullptr) {
            continue;
        }
        bool playing = false;
        for (int32_t j = 0; j < M_MAX_PLAYERS; j++) {
            const M_PLAYER *const player = &m_Net.players[j];
            playing |= player->known && player->profile.data != nullptr
                && memcmp(player->id, saved->id, COOP_ID_SIZE) == 0;
        }
        if (!playing) {
            M_WriteProfile(io, saved->id, saved->name, &saved->profile);
            written++;
        }
    }
    JSONW_POP_AND_SET(io, "players");
    JSONW_POP_AND_SET(io, "coop");
}

RESULT Coop_Net_LoadProfiles(JSON_READ_IO *const io)
{
    M_ForgetSaved();
    // A save made out of co-op has none.
    if (!JSON_ReadIO_HasKey(io, "coop")) {
        return OK;
    }
    MUST(JSON_PUSH(io, "coop"));
    MUST(JSON_PUSH(io, "players"));
    const int32_t count = MIN(JSON_ARRAY_LEN(io), M_MAX_SAVED_PROFILES);
    for (int32_t i = 0; i < count; i++) {
        MUST(JSON_PUSH_INDEX(io, i));
        const char *id_hex = nullptr;
        const char *name = nullptr;
        const char *profile_hex = nullptr;
        MUST(JSON_READ(io, "id", &id_hex));
        MUST(JSON_READ(io, "name", &name));
        MUST(JSON_READ(io, "profile", &profile_hex));
        MUST(JSON_POP(io));

        M_SAVED_PROFILE *const saved = &m_Net.saved[i];
        const size_t size = strlen(profile_hex) / 2;
        if (size == 0 || size > COOP_PROFILE_MAX_SIZE
            || !M_FromHex(id_hex, saved->id, COOP_ID_SIZE)) {
            continue;
        }
        uint8_t *const data = Memory_Alloc(size);
        if (!M_FromHex(profile_hex, data, size)) {
            Memory_Free(data);
            continue;
        }
        saved->profile = (M_BLOB) { .data = data, .size = size };
        Coop_Identity_CleanName(name, saved->name);
        saved->pending = true;
    }
    MUST(JSON_POP(io));
    MUST(JSON_POP(io));
    return OK;
}

// Revive ----------------------------------------------------------------------

// Whether another player is down in this level: connected, here, and out of
// health.
static bool M_IsDeadTeammate(const int32_t index, const int32_t level_num)
{
    const M_PLAYER *const player = M_GetPlayer(index);
    return player != nullptr && index != m_Net.local_index
        && player->connected && player->has_state
        && player->level_num == level_num && player->state.hit_points <= 0;
}

static bool M_IsLivingTeammate(const int32_t index, const int32_t level_num)
{
    const M_PLAYER *const player = M_GetPlayer(index);
    return player != nullptr && index != m_Net.local_index
        && player->connected && player->has_state
        && player->level_num == level_num && player->state.hit_points > 0;
}

static void M_PutRevive(
    uint8_t **const cursor, const M_PACKET_TYPE type, const int32_t index,
    const int16_t room_num, const XYZ_32 pos)
{
    M_PutHeader(cursor, type);
    Coop_Wire_PutU8(cursor, (uint8_t)index);
    Coop_Wire_PutS16(cursor, room_num);
    Coop_Wire_PutS32(cursor, pos.x);
    Coop_Wire_PutS32(cursor, pos.y);
    Coop_Wire_PutS32(cursor, pos.z);
}

static void M_GetRevive(
    const ENetPacket *const packet, int32_t *const index,
    int16_t *const room_num, XYZ_32 *const pos)
{
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    *index = Coop_Wire_GetU8(&cursor);
    *room_num = Coop_Wire_GetS16(&cursor);
    pos->x = Coop_Wire_GetS32(&cursor);
    pos->y = Coop_Wire_GetS32(&cursor);
    pos->z = Coop_Wire_GetS32(&cursor);
}

// Brings the player at `target` back at `pos`, on the host's word: here if
// that is the host, else by telling them.
static void M_HostRevive(
    const int32_t target, const int32_t reviver, const int16_t room_num,
    const XYZ_32 pos)
{
    if (target == M_HOST_INDEX) {
        m_Net.revive_pending = true;
        m_Net.revived_by = reviver;
        m_Net.revive_room = room_num;
        m_Net.revive_pos = pos;
        return;
    }
    uint8_t data[M_REVIVE_SIZE];
    uint8_t *cursor = data;
    M_PutRevive(&cursor, M_PACKET_REVIVE, reviver, room_num, pos);
    M_Send(
        m_Net.players[target].peer, M_CHANNEL_RELIABLE, data, sizeof(data),
        ENET_PACKET_FLAG_RELIABLE);
}

static void M_ReceiveReviveRequest(
    ENetPeer *const peer, const ENetPacket *const packet)
{
    if (!M_IsHost() || packet->dataLength != M_REVIVE_SIZE) {
        return;
    }
    int32_t target;
    int16_t room_num;
    XYZ_32 pos;
    M_GetRevive(packet, &target, &room_num, &pos);
    const int32_t reviver = M_GetPeerIndex(peer);
    const M_PLAYER *const reviver_player = M_GetPlayer(reviver);
    if (reviver_player == nullptr) {
        return;
    }
    // Only a player that is down comes back. The host is not among the
    // others it sees, so it checks its own Lara for itself.
    const int32_t level_num = reviver_player->level_num;
    const bool target_down = target == M_HOST_INDEX
        ? M_GetLevelNum() == level_num && Lara_GetItem() != nullptr
            && Lara_GetItem()->hit_points <= 0
        : M_IsDeadTeammate(target, level_num);
    if (target_down) {
        M_HostRevive(target, reviver, room_num, pos);
    }
}

static void M_ReceiveRevive(const ENetPacket *const packet)
{
    if (M_IsHost() || packet->dataLength != M_REVIVE_SIZE) {
        return;
    }
    M_GetRevive(
        packet, &m_Net.revived_by, &m_Net.revive_room, &m_Net.revive_pos);
    m_Net.revive_pending = true;
}

// Applies a revive the host granted, now that the game is in play.
static void M_ApplyRevive(void)
{
    if (!m_Net.revive_pending) {
        return;
    }
    m_Net.revive_pending = false;
    if (Lara_GetItem() == nullptr || Lara_GetItem()->hit_points > 0) {
        return;
    }
    if (Lara_Cheat_Revive(m_Net.revive_pos, m_Net.revive_room)) {
        const M_PLAYER *const reviver = M_GetPlayer(m_Net.revived_by);
        Console_Info(
            "Coop: %s revived you",
            reviver != nullptr ? reviver->name : "a teammate");
        m_Net.told_waiting = false;
    }
}

// Says when another player goes down, and tells a fallen one it waits.
static void M_WatchDeaths(const int32_t level_num)
{
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        const bool alive = M_IsLivingTeammate(i, level_num);
        if (m_Net.was_alive[i] && M_IsDeadTeammate(i, level_num)) {
            Console_Info("Coop: %s is down", m_Net.players[i].name);
        }
        m_Net.was_alive[i] = alive;
    }

    const ITEM *const lara_item = Lara_GetItem();
    if (lara_item == nullptr || lara_item->hit_points > 0) {
        m_Net.told_waiting = false;
    } else if (!m_Net.told_waiting && Coop_Net_ShouldHoldDeath()) {
        Console_Info("Coop: waiting for a teammate to revive you");
        m_Net.told_waiting = true;
    }
}

bool Coop_Net_ShouldHoldDeath(void)
{
    if (m_Net.host == nullptr) {
        return false;
    }
    // A client waits for the host either way: to be revived, or to be taken
    // along when the host loads once everyone is down.
    if (!M_IsHost()) {
        return true;
    }
    const int32_t level_num = M_GetLevelNum();
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        if (M_IsLivingTeammate(i, level_num)) {
            return true;
        }
    }
    return false;
}

int32_t Coop_Net_GetDeadTeammates(COOP_PLAYER_INFO *const out)
{
    const int32_t level_num = M_GetLevelNum();
    int32_t count = 0;
    if (m_Net.host == nullptr || level_num < 0) {
        return 0;
    }
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        if (!M_IsDeadTeammate(i, level_num)) {
            continue;
        }
        COOP_PLAYER_INFO *const info = &out[count++];
        *info = (COOP_PLAYER_INFO) { .number = i + 1, .connected = true };
        strcpy(info->name, m_Net.players[i].name);
    }
    return count;
}

bool Coop_Net_Revive(const int32_t number)
{
    const int32_t target = number - 1;
    const int32_t level_num = M_GetLevelNum();
    ITEM *const lara_item = Lara_GetItem();
    if (lara_item == nullptr || lara_item->hit_points <= 0
        || !M_IsDeadTeammate(target, level_num)) {
        return false;
    }

    if (M_IsHost()) {
        M_HostRevive(target, M_HOST_INDEX, lara_item->room_num, lara_item->pos);
    } else {
        uint8_t data[M_REVIVE_SIZE];
        uint8_t *cursor = data;
        M_PutRevive(
            &cursor, M_PACKET_REVIVE_REQUEST, target, lara_item->room_num,
            lara_item->pos);
        M_Send(
            m_Net.host_peer, M_CHANNEL_RELIABLE, data, sizeof(data),
            ENET_PACKET_FLAG_RELIABLE);
    }
    Console_Info("Coop: you revived %s", m_Net.players[target].name);
    return true;
}

// Exit ------------------------------------------------------------------------

static bool M_IsHostAlive(void)
{
    const ITEM *const lara_item = Lara_GetItem();
    return lara_item != nullptr && lara_item->hit_points > 0;
}

// How many of the living players in the host's level are at its exit, and
// how many living players there are. The fallen do not hold the others back:
// they come along to the next level.
static void M_CountAtExit(
    const int32_t level_num, int32_t *const at, int32_t *const total)
{
    *at = 0;
    *total = 0;
    if (M_IsHostAlive() || m_Net.players[M_HOST_INDEX].at_exit) {
        (*total)++;
        *at += m_Net.players[M_HOST_INDEX].at_exit ? 1 : 0;
    }
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        if (i == M_HOST_INDEX) {
            continue;
        }
        const M_PLAYER *const player = &m_Net.players[i];
        if (player->connected && player->has_state
            && player->level_num == level_num
            && (player->state.hit_points > 0 || player->at_exit)) {
            (*total)++;
            *at += player->at_exit ? 1 : 0;
        }
    }
}

static void M_ForgetExit(void)
{
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        m_Net.players[i].at_exit = false;
    }
    m_Net.exit_count = 0;
    m_Net.exit_total = 0;
    m_Net.exit_mask = 0;
    m_Net.exit_reminder_wait = 0;
}

static void M_ReceiveAtExit(
    ENetPeer *const peer, const ENetPacket *const packet)
{
    M_PLAYER *const player = M_GetPlayer(M_GetPeerIndex(peer));
    if (!M_IsHost() || player == nullptr
        || packet->dataLength != M_AT_EXIT_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    if (Coop_Wire_GetS32(&cursor) == M_GetLevelNum()) {
        player->at_exit = true;
    }
}

static void M_ShowExitStatus(const int32_t at, const int32_t total)
{
    if (at > 0 && at < total) {
        Console_Info("Coop: %d/%d players at the exit", at, total);
    }
}

static void M_ReceiveExitStatus(const ENetPacket *const packet)
{
    if (M_IsHost() || packet->dataLength != M_EXIT_STATUS_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data + M_HEADER_SIZE;
    const int32_t at = Coop_Wire_GetU8(&cursor);
    const int32_t total = Coop_Wire_GetU8(&cursor);
    const uint8_t mask = Coop_Wire_GetU8(&cursor);
    m_Net.exit_count = at;
    m_Net.exit_total = total;
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        m_Net.players[i].at_exit = (mask & (1 << i)) != 0;
    }
    M_ShowExitStatus(at, total);
}

// On the host, each frame: tells everyone how many are at the exit, and who,
// whenever that changes.
static void M_CheckExit(const int32_t level_num)
{
    int32_t at;
    int32_t total;
    M_CountAtExit(level_num, &at, &total);
    uint8_t mask = 0;
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        mask |= m_Net.players[i].at_exit ? (1 << i) : 0;
    }
    if (at == m_Net.exit_count && total == m_Net.exit_total
        && mask == m_Net.exit_mask) {
        return;
    }
    m_Net.exit_count = at;
    m_Net.exit_total = total;
    m_Net.exit_mask = mask;
    M_ShowExitStatus(at, total);
    uint8_t data[M_EXIT_STATUS_SIZE];
    uint8_t *cursor = data;
    M_PutHeader(&cursor, M_PACKET_EXIT_STATUS);
    Coop_Wire_PutU8(&cursor, (uint8_t)at);
    Coop_Wire_PutU8(&cursor, (uint8_t)total);
    Coop_Wire_PutU8(&cursor, mask);
    ASSERT(cursor == data + sizeof(data));
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        if (i != M_HOST_INDEX && m_Net.players[i].connected) {
            M_Send(
                m_Net.players[i].peer, M_CHANNEL_RELIABLE, data, sizeof(data),
                ENET_PACKET_FLAG_RELIABLE);
        }
    }
}

void Coop_Net_OnReachExit(void)
{
    const int32_t level_num = M_GetLevelNum();
    if (m_Net.host == nullptr || level_num < 0) {
        return;
    }
    m_Net.exit_reminder_wait = 0;
    if (M_IsHost()) {
        m_Net.players[M_HOST_INDEX].at_exit = true;
        Coop_Creatures_SetLaraAway(true);
        M_CheckExit(level_num);
        enet_host_flush(m_Net.host);
        return;
    }
    if (!m_Net.told_at_exit) {
        m_Net.told_at_exit = true;
        uint8_t data[M_AT_EXIT_SIZE];
        uint8_t *cursor = data;
        M_PutHeader(&cursor, M_PACKET_AT_EXIT);
        Coop_Wire_PutS32(&cursor, level_num);
        M_Send(
            m_Net.host_peer, M_CHANNEL_RELIABLE, data, sizeof(data),
            ENET_PACKET_FLAG_RELIABLE);
        enet_host_flush(m_Net.host);
    }
}

bool Coop_Net_HoldsLevelStats(void)
{
    if (m_Net.host == nullptr) {
        return false;
    }
    // A client waits for the host to say where to go next; the host for
    // every living player to reach the exit.
    if (!M_IsHost()) {
        return !m_Net.start_pending;
    }
    const int32_t level_num = M_GetLevelNum();
    if (level_num < 0) {
        return false;
    }
    int32_t at;
    int32_t total;
    M_CountAtExit(level_num, &at, &total);
    return at < total;
}

static void M_UpdatePuppets(int32_t level_num);

void Coop_Net_TickLevelStats(void)
{
    const int32_t level_num = M_GetLevelNum();
    if (m_Net.host == nullptr || level_num < 0) {
        return;
    }
    // The host's level goes on for those still playing it, with its Lara out
    // of it.
    if (M_IsHost() && m_Net.players[M_HOST_INDEX].at_exit) {
        Game_TickBeginFrame();
        Game_TickWorld();
        M_SyncWorld(level_num);
        M_SyncCreatures(level_num);
        M_CheckExit(level_num);
        M_UpdatePuppets(level_num);
    }
    if (Coop_Net_HoldsLevelStats() && m_Net.exit_reminder_wait-- <= 0) {
        m_Net.exit_reminder_wait = M_EXIT_REMINDER_FRAMES;
        if (m_Net.exit_total > 0) {
            Console_Info(
                "Coop: waiting for the other players (%d/%d at the exit)",
                m_Net.exit_count, m_Net.exit_total);
        } else {
            Console_Info("Coop: waiting for the other players");
        }
    }
    enet_host_flush(m_Net.host);
}

// Connection ------------------------------------------------------------------

static bool M_StartHost(const uint16_t port)
{
    const ENetAddress address = { .host = ENET_HOST_ANY, .port = port };
    m_Net.host = enet_host_create(
        &address, M_MAX_PLAYERS - 1, M_CHANNEL_COUNT, 0, 0);
    if (m_Net.host == nullptr) {
        Console_Info("Coop: cannot host on port %d", port);
        return false;
    }
    Console_Info(
        "Coop: %s hosting on port %d", Coop_Identity_Get()->name, port);
    return true;
}

static bool M_StartClient(const char *const target, const uint16_t port)
{
    m_Net.host = enet_host_create(nullptr, 1, M_CHANNEL_COUNT, 0, 0);
    if (m_Net.host == nullptr) {
        Console_Info("Coop: cannot open a network connection");
        return false;
    }

    m_Net.host_address = (ENetAddress) { .port = port };
    if (enet_address_set_host(&m_Net.host_address, target) != 0) {
        Console_Info("Coop: cannot find %s", target);
        return false;
    }
    Console_Info(
        "Coop: %s joining %s:%d", Coop_Identity_Get()->name, target, port);
    return true;
}

static bool M_Start(const M_ROLE role, const char *const address)
{
    if (enet_initialize() != 0) {
        LOG_ERROR("Coop: ENet failed to start");
        return false;
    }
    m_Net.running = true;
    m_Net.role = role;
    m_Net.world_changes = Vector_Create(sizeof(COOP_ITEM_RECORD));
    m_Net.world_taken = Vector_Create(sizeof(COOP_ITEM_RECORD));
    m_Net.creatures_moving = Vector_Create(sizeof(COOP_CREATURE_RECORD));
    m_Net.creatures_changed = Vector_Create(sizeof(COOP_CREATURE_RECORD));
    m_Net.creature_effects = Vector_Create(sizeof(COOP_EFFECT_RECORD));
    m_Net.creature_sounds = Vector_Create(sizeof(COOP_SOUND_RECORD));
    M_SetUpLocalPlayer();

    const uint16_t port = M_GetPort();
    if (role == M_ROLE_HOST) {
        return M_StartHost(port);
    }
    return M_StartClient(address, port);
}

// Ask the host to let this client in. The answer comes as a connect event,
// or as a disconnect one if the host cannot be reached.
static void M_Connect(void)
{
    m_Net.host_peer = enet_host_connect(
        m_Net.host, &m_Net.host_address, M_CHANNEL_COUNT, M_PROTOCOL_VERSION);
    if (m_Net.host_peer != nullptr) {
        enet_peer_timeout(
            m_Net.host_peer, 0, M_PEER_TIMEOUT_MIN_MS, M_PEER_TIMEOUT_MAX_MS);
    }
    m_Net.reconnect_wait = M_RECONNECT_FRAMES;
}

static void M_OnConnect(ENetPeer *const peer, const uint32_t data)
{
    enet_peer_timeout(peer, 0, M_PEER_TIMEOUT_MIN_MS, M_PEER_TIMEOUT_MAX_MS);
    if (!M_IsHost()) {
        // The host's answer. Say who this is; the roster says the rest.
        m_Net.host_connected = true;
        Console_Info("Coop: connected");
        M_SendHello();
        return;
    }
    // The version rides in the connect request. A player is only counted in
    // once it has said who it is.
    peer->data = nullptr;
    if (data != M_PROTOCOL_VERSION) {
        LOG_WARNING(
            "Coop: turned away a player on protocol %u (ours is %d)", data,
            M_PROTOCOL_VERSION);
        enet_peer_disconnect_now(peer, M_END_REFUSED);
    }
}

static void M_OnDisconnect(ENetPeer *const peer, const uint32_t data)
{
    if (!M_IsHost()) {
        if (peer != m_Net.host_peer) {
            return;
        }
        // Over on purpose: nothing to try again.
        if (data == M_END_HOST_LEFT || data == M_END_REFUSED) {
            m_Net.ended = data;
        }
        // Without the host there is nobody else either. Try it again.
        const bool was_connected = m_Net.host_connected;
        m_Net.host_peer = nullptr;
        m_Net.host_connected = false;
        m_Net.world_synced = false;
        m_Net.local_index = -1;
        // Back with the host, the next start it tells is news again.
        m_Net.heard_load_count = -1;
        for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
            m_Net.players[i].connected = false;
            M_ForgetPlayerState(i);
        }
        if (was_connected && m_Net.ended == M_END_DROPPED) {
            Console_Info("Coop: lost the host, trying again");
        }
        return;
    }

    const int32_t index = M_GetPeerIndex(peer);
    peer->data = nullptr;
    M_PLAYER *const player = M_GetPlayer(index);
    if (player == nullptr || player->peer != peer) {
        return;
    }
    player->connected = false;
    player->peer = nullptr;
    M_ForgetPlayerState(index);
    Console_Info("Coop: %s left", player->name);
    M_BroadcastRoster();
}

static void M_OnReceive(ENetPeer *const peer, const ENetPacket *const packet)
{
    if (packet->dataLength < M_HEADER_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data;
    if (Coop_Wire_GetU8(&cursor) != M_PROTOCOL_VERSION) {
        return;
    }
    const uint8_t *const end = packet->data + packet->dataLength;
    const uint8_t type = Coop_Wire_GetU8(&cursor);

    // On the host, only a player who has said who it is gets a hearing.
    if (M_IsHost() && type != M_PACKET_HELLO && M_GetPeerIndex(peer) < 0) {
        return;
    }
    switch (type) {
    case M_PACKET_PLAYER_STATE:
        M_ReceivePlayerState(peer, packet);
        break;
    case M_PACKET_WORLD:
        M_ReceiveWorld(peer, packet);
        break;
    case M_PACKET_WORLD_REQUEST:
        M_ReceiveWorldRequest(peer, packet);
        break;
    case M_PACKET_HELLO:
        M_ReceiveHello(peer, cursor, end);
        break;
    case M_PACKET_ROSTER:
        M_ReceiveRoster(cursor, end);
        break;
    case M_PACKET_START:
        M_ReceiveStart(packet);
        break;
    case M_PACKET_PROFILE:
        M_ReceiveProfile(peer, packet);
        break;
    case M_PACKET_PROFILE_RESTORE:
        M_ReceiveRestore(packet);
        break;
    case M_PACKET_REVIVE_REQUEST:
        M_ReceiveReviveRequest(peer, packet);
        break;
    case M_PACKET_REVIVE:
        M_ReceiveRevive(packet);
        break;
    case M_PACKET_AT_EXIT:
        M_ReceiveAtExit(peer, packet);
        break;
    case M_PACKET_EXIT_STATUS:
        M_ReceiveExitStatus(packet);
        break;
    case M_PACKET_CREATURES:
        M_ReceiveCreatures(packet);
        break;
    case M_PACKET_HIT:
        M_ReceiveHit(packet);
        break;
    case M_PACKET_DAMAGE:
        M_ReceiveDamage(packet);
        break;
    case M_PACKET_ACTIVATE:
        M_ReceiveActivate(packet);
        break;
    case M_PACKET_CREATURE_EVENTS:
        M_ReceiveCreatureEvents(packet);
        break;
    default:
        break;
    }
}

static void M_Service(void)
{
    ENetEvent event;
    while (enet_host_service(m_Net.host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            M_OnConnect(event.peer, event.data);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            M_OnReceive(event.peer, event.packet);
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            M_OnDisconnect(event.peer, event.data);
            break;
        default:
            break;
        }
    }
}

static void M_Shutdown(void);

// The other players leave the level with the session. Only done when a
// session ends in play: at shutdown the level's items are not ours to touch.
static void M_RemovePuppets(void)
{
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        Coop_Puppet_Remove(i);
    }
}

// Ends whatever session there is, then starts the one asked for. One that
// cannot start leaves nothing behind.
static bool M_Begin(const M_ROLE role, const char *const address)
{
    M_RemovePuppets();
    M_Shutdown();
    if (!M_Start(role, address)) {
        M_Shutdown();
        Console_Info("Coop: the network could not start");
        return false;
    }
    return true;
}

bool Coop_Net_IsEnabled(void)
{
    return m_Net.role != M_ROLE_NONE;
}

bool Coop_Net_IsHost(void)
{
    return M_IsHost();
}

COOP_NET_STATUS Coop_Net_GetStatus(void)
{
    switch (m_Net.role) {
    case M_ROLE_HOST:
        return COOP_NET_HOSTING;
    case M_ROLE_CLIENT:
        return m_Net.host_connected ? COOP_NET_CONNECTED : COOP_NET_CONNECTING;
    default:
        return COOP_NET_OFF;
    }
}

bool Coop_Net_Host(void)
{
    return M_Begin(M_ROLE_HOST, nullptr);
}

bool Coop_Net_Join(const char *const address)
{
    if (address == nullptr || address[0] == '\0') {
        return false;
    }
    Coop_Identity_SetLastAddress(address);
    return M_Begin(M_ROLE_CLIENT, Coop_Identity_GetLastAddress());
}

// The peers not yet through with disconnecting: one told to go is no longer
// connected, but its goodbye, and the reason it carries, is still on its way
// until it is acknowledged.
static int32_t M_CountLingeringPeers(void)
{
    int32_t count = 0;
    for (size_t i = 0; i < m_Net.host->peerCount; i++) {
        const ENetPeerState state = m_Net.host->peers[i].state;
        count += state != ENET_PEER_STATE_DISCONNECTED
            && state != ENET_PEER_STATE_ZOMBIE;
    }
    return count;
}

// Leaving for good waits a moment for what was sent last to land, rather
// than drop it on the way out: a client's profile, or the host's word to
// each client that the game is over.
static void M_DisconnectGracefully(void)
{
    if (m_Net.host == nullptr) {
        return;
    }
    if (M_IsHost()) {
        for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
            const M_PLAYER *const player = &m_Net.players[i];
            if (i != M_HOST_INDEX && M_IsPeerConnected(player->peer)) {
                enet_peer_disconnect_later(player->peer, M_END_HOST_LEFT);
            }
        }
    } else if (M_IsPeerConnected(m_Net.host_peer)) {
        enet_peer_disconnect_later(m_Net.host_peer, M_END_DROPPED);
    }

    const enet_uint32 start = enet_time_get();
    ENetEvent event;
    while (M_CountLingeringPeers() > 0
           && enet_time_get() - start < M_LEAVE_WAIT_MS) {
        if (enet_host_service(m_Net.host, &event, 50) > 0
            && event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        }
    }
}

// On a client the host let go of: the session is over, and a player out in a
// level the host left goes back to the title.
static void M_EndSession(void)
{
    const M_END_REASON reason = m_Net.ended;
    Console_Info(
        reason == M_END_HOST_LEFT
            ? "Coop: the host ended the game"
            : "Coop: the host turned you away (game full, or another version)");
    M_RemovePuppets();
    M_Shutdown();
    if (reason == M_END_HOST_LEFT && Coop_IsPlayableLevel()) {
        GF_OverrideCommand(
            (GF_COMMAND) { .action = GF_EXIT_TO_TITLE },
            !UI_Takeover_IsAnyHeld());
    }
}

void Coop_Net_Leave(void)
{
    if (m_Net.role != M_ROLE_NONE) {
        Console_Info("Coop: left the game");
    }
    if (Coop_IsPlayableLevel()) {
        M_SendProfile(true);
    }
    M_DisconnectGracefully();
    M_RemovePuppets();
    M_Shutdown();
}

void Coop_Net_OnLevelEnding(void)
{
    // What Lara carries out of the level, for the host to keep and to hand
    // back in the next one.
    if (m_Net.host != nullptr && m_Net.world_synced) {
        M_SendProfile(true);
        enet_host_flush(m_Net.host);
    }
}

int32_t Coop_Net_GetPlayers(COOP_PLAYER_INFO *const out)
{
    int32_t count = 0;
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        const M_PLAYER *const player = &m_Net.players[i];
        if (!player->known) {
            continue;
        }
        COOP_PLAYER_INFO *const info = &out[count++];
        *info = (COOP_PLAYER_INFO) {
            .number = i + 1,
            .connected = player->connected,
            .is_local = i == m_Net.local_index,
        };
        strcpy(info->name, player->name);
    }
    return count;
}

void Coop_Net_Service(void)
{
    if (!m_ArgsTaken) {
        m_ArgsTaken = true;
        const SHELL_ARGS *const args = Shell_GetArgs();
        if (args->coop.host) {
            Coop_Net_Host();
        } else if (args->coop.join != nullptr) {
            M_Begin(M_ROLE_CLIENT, args->coop.join);
        }
    }
    if (m_Net.host == nullptr) {
        return;
    }

    M_Service();
    if (m_Net.ended != M_END_DROPPED) {
        M_EndSession();
        return;
    }
    if (!M_IsHost() && m_Net.host_peer == nullptr
        && m_Net.reconnect_wait-- <= 0) {
        M_Connect();
    }
    enet_host_flush(m_Net.host);
}

void Coop_Net_OnTitle(void)
{
    // Back at the title, a player has left the game: a client goes, and the
    // host ends it for everyone. Joining again picks up where the host is.
    if (m_Net.host != nullptr && m_Net.played) {
        Coop_Net_Leave();
    }
}

void Coop_Net_Control(void)
{
    if (m_Net.host == nullptr) {
        return;
    }
    const int32_t level_num = M_GetLevelNum();
    if (level_num < 0) {
        return;
    }
    m_Net.played = true;

    if (M_IsHost()) {
        // Normally told as the level's sequence starts; this catches a level
        // reached any other way.
        M_AnnounceLevel(level_num, false);
    } else {
        m_Net.start_pending = false;
    }
    M_ApplyRevive();
    M_ApplyDamage();
    M_SendLocalState(level_num);
    M_SyncWorld(level_num);
    M_SyncCreatures(level_num);
    M_SyncProfile();
    M_WatchDeaths(level_num);
    if (M_IsHost()) {
        M_CheckExit(level_num);
    }

    M_UpdatePuppets(level_num);
    enet_host_flush(m_Net.host);
}

// Each other player appears while playing the same level, and is gone once at
// its exit.
static void M_UpdatePuppets(const int32_t level_num)
{
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        const M_PLAYER *const player = &m_Net.players[i];
        if (i == m_Net.local_index) {
            continue;
        }
        if (player->connected && player->has_state
            && player->level_num == level_num && !player->at_exit) {
            Coop_Puppet_SetName(i, player->name);
            Coop_Puppet_Apply(i, &player->state);
        } else {
            Coop_Puppet_Remove(i);
        }
    }
}

void Coop_Net_OnLevelUnload(void)
{
    // Whatever the new level, or the same one loaded afresh, it has to catch
    // up with the host's again.
    m_Net.world_synced = false;
    m_Net.world_request_wait = 0;
    m_Net.told_at_exit = false;
    m_Net.damage_pending = 0;
    m_Net.damage_hit_status = false;
    m_Net.damage_fatal = false;
    M_ForgetExit();
}

void Coop_Net_OnLevelSequence(const int32_t level_num)
{
    if (M_IsHost() && m_Net.host != nullptr) {
        M_AnnounceLevel(level_num, true);
        enet_host_flush(m_Net.host);
    }
}

bool Coop_Net_IsStartPending(void)
{
    return m_Net.start_pending;
}

static void M_Shutdown(void)
{
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        M_FreeBlob(&m_Net.players[i].profile);
    }
    M_ForgetSaved();
    M_FreeBlob(&m_Net.profile_sent);
    M_FreeBlob(&m_Net.profile_restore);
    if (m_Net.world_changes != nullptr) {
        Vector_Free(m_Net.world_changes);
    }
    if (m_Net.world_taken != nullptr) {
        Vector_Free(m_Net.world_taken);
    }
    if (m_Net.creatures_moving != nullptr) {
        Vector_Free(m_Net.creatures_moving);
    }
    if (m_Net.creatures_changed != nullptr) {
        Vector_Free(m_Net.creatures_changed);
    }
    if (m_Net.creature_effects != nullptr) {
        Vector_Free(m_Net.creature_effects);
    }
    if (m_Net.creature_sounds != nullptr) {
        Vector_Free(m_Net.creature_sounds);
    }
    if (m_Net.host != nullptr) {
        // A host going ends the game for everyone, even one that never
        // acknowledged hearing so.
        const M_END_REASON reason =
            M_IsHost() ? M_END_HOST_LEFT : M_END_DROPPED;
        for (size_t i = 0; i < m_Net.host->peerCount; i++) {
            ENetPeer *const peer = &m_Net.host->peers[i];
            if (peer->state != ENET_PEER_STATE_DISCONNECTED) {
                enet_peer_disconnect_now(peer, reason);
            }
        }
        enet_host_destroy(m_Net.host);
    }
    if (m_Net.running) {
        enet_deinitialize();
    }
    m_Net = (typeof(m_Net)) {
        .local_index = -1,
        .announced_level = -1,
        .heard_load_count = -1,
    };
}

static void M_OnGameExit(void)
{
    M_DisconnectGracefully();
    M_Shutdown();
}

REGISTER_SUBSYSTEM(.shutdown = M_OnGameExit)
