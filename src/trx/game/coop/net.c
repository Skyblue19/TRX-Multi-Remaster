#include <trx/game/coop/net.h>

#include <trx/core/log.h>
#include <trx/core/subsystem.h>
#include <trx/debug.h>
#include <trx/game/console.h>
#include <trx/game/coop/common.h>
#include <trx/game/coop/identity.h>
#include <trx/game/coop/puppet.h>
#include <trx/game/coop/wire.h>
#include <trx/game/coop/world.h>
#include <trx/game/game_flow.h>
#include <trx/game/savegame/manager.h>
#include <trx/game/shell.h>
#include <trx/game/ui/dialogs/takeover.h>

#include <enet/enet.h>

#include <stdint.h>
#include <string.h>

// Bumped whenever a packet changes shape. A peer on another version is turned
// away when it connects, rather than misreading every packet after.
#define M_PROTOCOL_VERSION 6
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
} M_PACKET_TYPE;

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
} M_PLAYER;

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
    // catch up with the host's again.
    if (level_num == M_GetLevelNum()) {
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
        enet_peer_disconnect(peer, 0);
        return;
    }

    const int32_t index = M_PickIndexFor(id);
    if (index < 0) {
        LOG_WARNING("Coop: turned away %s, the game is full", name);
        enet_peer_disconnect(peer, 0);
        return;
    }

    M_PLAYER *const player = &m_Net.players[index];
    *player = (M_PLAYER) {
        .known = true,
        .connected = true,
        .peer = peer,
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
        enet_peer_disconnect_now(peer, 0);
    }
}

static void M_OnDisconnect(ENetPeer *const peer)
{
    if (!M_IsHost()) {
        if (peer != m_Net.host_peer) {
            return;
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
        if (was_connected) {
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
            M_OnDisconnect(event.peer);
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

void Coop_Net_Leave(void)
{
    if (m_Net.role != M_ROLE_NONE) {
        Console_Info("Coop: left the game");
    }
    M_RemovePuppets();
    M_Shutdown();
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
    if (!M_IsHost() && m_Net.host_peer == nullptr
        && m_Net.reconnect_wait-- <= 0) {
        M_Connect();
    }
    enet_host_flush(m_Net.host);
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

    if (M_IsHost()) {
        // Normally told as the level's sequence starts; this catches a level
        // reached any other way.
        M_AnnounceLevel(level_num, false);
    } else {
        m_Net.start_pending = false;
    }
    M_SendLocalState(level_num);
    M_SyncWorld(level_num);

    // Each other player appears while in the same level.
    for (int32_t i = 0; i < M_MAX_PLAYERS; i++) {
        const M_PLAYER *const player = &m_Net.players[i];
        if (i == m_Net.local_index) {
            continue;
        }
        if (player->connected && player->has_state
            && player->level_num == level_num) {
            Coop_Puppet_SetName(i, player->name);
            Coop_Puppet_Apply(i, &player->state);
        } else {
            Coop_Puppet_Remove(i);
        }
    }

    enet_host_flush(m_Net.host);
}

void Coop_Net_OnLevelUnload(void)
{
    // Whatever the new level, or the same one loaded afresh, it has to catch
    // up with the host's again.
    m_Net.world_synced = false;
    m_Net.world_request_wait = 0;
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
    if (m_Net.world_changes != nullptr) {
        Vector_Free(m_Net.world_changes);
    }
    if (m_Net.world_taken != nullptr) {
        Vector_Free(m_Net.world_taken);
    }
    if (m_Net.host != nullptr) {
        for (size_t i = 0; i < m_Net.host->peerCount; i++) {
            ENetPeer *const peer = &m_Net.host->peers[i];
            if (peer->state != ENET_PEER_STATE_DISCONNECTED) {
                enet_peer_disconnect_now(peer, 0);
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

REGISTER_SUBSYSTEM(.shutdown = M_Shutdown)
