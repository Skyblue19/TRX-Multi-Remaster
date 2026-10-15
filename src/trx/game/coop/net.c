#include <trx/game/coop/net.h>

#include <trx/core/log.h>
#include <trx/core/subsystem.h>
#include <trx/debug.h>
#include <trx/game/console.h>
#include <trx/game/coop/common.h>
#include <trx/game/coop/puppet.h>
#include <trx/game/coop/world.h>
#include <trx/game/game_flow.h>
#include <trx/game/shell.h>

#include <enet/enet.h>

#include <stdint.h>

// Bumped whenever a packet changes shape. A peer on another version is turned
// away when it connects, rather than misreading every packet after.
#define M_PROTOCOL_VERSION 3
#define M_CHANNEL_COUNT 2
// Player states: unreliable, as each one replaces the last.
#define M_CHANNEL_STATE 0
// The world: reliable and in order, as each change builds on the last.
#define M_CHANNEL_WORLD 1
// How often a game not yet caught up with the host's level asks again.
#define M_WORLD_REQUEST_FRAMES 30
// The host takes one other player for now.
#define M_MAX_PEERS 1
// The other player's puppet.
#define M_REMOTE_SLOT 0
// A level loads in one go, with nothing taken in meanwhile, so a peer is only
// given up on after longer than any load takes.
#define M_PEER_TIMEOUT_MIN_MS 30000
#define M_PEER_TIMEOUT_MAX_MS 60000
// How often a client that lost its host tries it again.
#define M_RECONNECT_FRAMES 90

typedef enum {
    M_PACKET_PLAYER_STATE = 1,
    // What changed in a level, or all of it.
    M_PACKET_WORLD = 2,
    // A client asking the host for all of its level.
    M_PACKET_WORLD_REQUEST = 3,
} M_PACKET_TYPE;

// version, type, level, whether it is all of the level, whether the flips
// follow, and the item count.
#define M_WORLD_HEADER_SIZE (1 + 1 + 4 + 1 + 1 + 2)
// groups, one-shot slots, a mask per slot
#define M_FLIP_SIZE (2 + 2 + MAX_FLIP_MAPS)
// item, object, flags, trigger mask, three anim states, anim, frame, timer,
// room, pos and rot
#define M_ITEM_RECORD_SIZE (2 + 2 + 2 + 1 + 3 * 2 + 2 + 2 + 2 + 2 + 3 * 4 + 6)
// version, type, level
#define M_WORLD_REQUEST_SIZE (1 + 1 + 4)

#define M_XYZ_16_SIZE (3 * 2)
// frame base, frame, anim, rot, flash
#define M_ARM_SIZE (4 + 2 + 2 + M_XYZ_16_SIZE + 2)
// visible, mesh, offset
#define M_EQUIPMENT_SIZE (1 + 4 + M_XYZ_16_SIZE)
// version, type and level, then the state: the body's pos, rot, room, anim
// and frame; the gun's status and type, head and torso rots, both arms and
// the flare; and what is worn on each body part.
#define M_PLAYER_STATE_SIZE                                                    \
    (1 + 1 + 4 + 3 * 4 + M_XYZ_16_SIZE + 2 + 2 + 2 + 2 + 2                    \
     + 2 * M_XYZ_16_SIZE + 2 * M_ARM_SIZE + 1                                  \
     + LM_NUMBER_OF * (4 + M_EQUIPMENT_SIZE))

static struct {
    bool started;
    bool running;
    ENetHost *host;
    // Where a client finds its host, kept to reconnect to.
    ENetAddress host_address;
    int32_t reconnect_wait;
    bool connected;
    // The other player. Set as soon as a client asks to connect, and on the
    // host once a client has.
    ENetPeer *peer;
    bool has_remote_state;
    int32_t remote_level_num;
    COOP_PLAYER_STATE remote_state;
    // On a client, whether this level has caught up with the host's since it
    // loaded. Until it has, what changes here is not worth sending.
    bool world_synced;
    int32_t world_request_wait;
    VECTOR *world_changes;
} m_Net;

// Packets are written out a field at a time, little end first, so that what
// goes on the wire does not hang on how either machine lays out a struct.
static void M_PutU8(uint8_t **const cursor, const uint8_t value)
{
    *(*cursor)++ = value;
}

static void M_PutS16(uint8_t **const cursor, const int16_t value)
{
    const uint16_t bits = (uint16_t)value;
    M_PutU8(cursor, bits & 0xFF);
    M_PutU8(cursor, bits >> 8);
}

static void M_PutS32(uint8_t **const cursor, const int32_t value)
{
    const uint32_t bits = (uint32_t)value;
    for (int32_t i = 0; i < 4; i++) {
        M_PutU8(cursor, (bits >> (8 * i)) & 0xFF);
    }
}

static uint8_t M_GetU8(const uint8_t **const cursor)
{
    return *(*cursor)++;
}

static int16_t M_GetS16(const uint8_t **const cursor)
{
    const uint16_t lo = M_GetU8(cursor);
    const uint16_t hi = M_GetU8(cursor);
    return (int16_t)(lo | (hi << 8));
}

static int32_t M_GetS32(const uint8_t **const cursor)
{
    uint32_t bits = 0;
    for (int32_t i = 0; i < 4; i++) {
        bits |= (uint32_t)M_GetU8(cursor) << (8 * i);
    }
    return (int32_t)bits;
}

static void M_PutXYZ16(uint8_t **const cursor, const XYZ_16 value)
{
    M_PutS16(cursor, value.x);
    M_PutS16(cursor, value.y);
    M_PutS16(cursor, value.z);
}

static XYZ_16 M_GetXYZ16(const uint8_t **const cursor)
{
    // Read one at a time: the order of initialisers is not the order of
    // evaluation.
    const int16_t x = M_GetS16(cursor);
    const int16_t y = M_GetS16(cursor);
    const int16_t z = M_GetS16(cursor);
    return (XYZ_16) { .x = x, .y = y, .z = z };
}

static void M_PutArm(uint8_t **const cursor, const COOP_ARM_STATE *const arm)
{
    M_PutS32(cursor, arm->frame_base);
    M_PutS16(cursor, arm->frame_num);
    M_PutS16(cursor, arm->anim_num);
    M_PutXYZ16(cursor, arm->rot);
    M_PutS16(cursor, arm->flash_gun);
}

static void M_GetArm(const uint8_t **const cursor, COOP_ARM_STATE *const arm)
{
    arm->frame_base = M_GetS32(cursor);
    arm->frame_num = M_GetS16(cursor);
    arm->anim_num = M_GetS16(cursor);
    arm->rot = M_GetXYZ16(cursor);
    arm->flash_gun = M_GetS16(cursor);
}

static void M_PutState(
    uint8_t **const cursor, const COOP_PLAYER_STATE *const state)
{
    M_PutS32(cursor, state->pos.x);
    M_PutS32(cursor, state->pos.y);
    M_PutS32(cursor, state->pos.z);
    M_PutXYZ16(cursor, state->rot);
    M_PutS16(cursor, state->room_num);
    M_PutS16(cursor, state->anim_num);
    M_PutS16(cursor, state->frame_num);
    M_PutS16(cursor, state->gun_status);
    M_PutS16(cursor, state->gun_type);
    M_PutXYZ16(cursor, state->head_rot);
    M_PutXYZ16(cursor, state->torso_rot);
    M_PutArm(cursor, &state->left_arm);
    M_PutArm(cursor, &state->right_arm);
    M_PutU8(cursor, state->flare_control ? 1 : 0);
    for (int32_t i = 0; i < LM_NUMBER_OF; i++) {
        M_PutS32(cursor, state->meshes[i]);
        const COOP_EQUIPMENT_STATE *const equipment = &state->equipment[i];
        M_PutU8(cursor, equipment->visible ? 1 : 0);
        M_PutS32(cursor, equipment->mesh);
        M_PutXYZ16(cursor, equipment->offset);
    }
}

static void M_GetState(
    const uint8_t **const cursor, COOP_PLAYER_STATE *const state)
{
    state->pos.x = M_GetS32(cursor);
    state->pos.y = M_GetS32(cursor);
    state->pos.z = M_GetS32(cursor);
    state->rot = M_GetXYZ16(cursor);
    state->room_num = M_GetS16(cursor);
    state->anim_num = M_GetS16(cursor);
    state->frame_num = M_GetS16(cursor);
    state->gun_status = M_GetS16(cursor);
    state->gun_type = M_GetS16(cursor);
    state->head_rot = M_GetXYZ16(cursor);
    state->torso_rot = M_GetXYZ16(cursor);
    M_GetArm(cursor, &state->left_arm);
    M_GetArm(cursor, &state->right_arm);
    state->flare_control = M_GetU8(cursor) != 0;
    for (int32_t i = 0; i < LM_NUMBER_OF; i++) {
        state->meshes[i] = M_GetS32(cursor);
        COOP_EQUIPMENT_STATE *const equipment = &state->equipment[i];
        equipment->visible = M_GetU8(cursor) != 0;
        equipment->mesh = M_GetS32(cursor);
        equipment->offset = M_GetXYZ16(cursor);
    }
}

static void M_PutItemRecord(
    uint8_t **const cursor, const COOP_ITEM_RECORD *const record)
{
    M_PutS16(cursor, record->item_num);
    M_PutS16(cursor, record->object_id);
    M_PutS16(cursor, (int16_t)record->flags);
    M_PutU8(cursor, record->trigger_mask);
    M_PutS16(cursor, record->current_anim_state);
    M_PutS16(cursor, record->goal_anim_state);
    M_PutS16(cursor, record->required_anim_state);
    M_PutS16(cursor, record->anim_num);
    M_PutS16(cursor, record->frame_num);
    M_PutS16(cursor, record->timer);
    M_PutS16(cursor, record->room_num);
    M_PutS32(cursor, record->pos.x);
    M_PutS32(cursor, record->pos.y);
    M_PutS32(cursor, record->pos.z);
    M_PutXYZ16(cursor, record->rot);
}

static void M_GetItemRecord(
    const uint8_t **const cursor, COOP_ITEM_RECORD *const record)
{
    record->item_num = M_GetS16(cursor);
    record->object_id = M_GetS16(cursor);
    record->flags = (uint16_t)M_GetS16(cursor);
    record->trigger_mask = M_GetU8(cursor);
    record->current_anim_state = M_GetS16(cursor);
    record->goal_anim_state = M_GetS16(cursor);
    record->required_anim_state = M_GetS16(cursor);
    record->anim_num = M_GetS16(cursor);
    record->frame_num = M_GetS16(cursor);
    record->timer = M_GetS16(cursor);
    record->room_num = M_GetS16(cursor);
    record->pos.x = M_GetS32(cursor);
    record->pos.y = M_GetS32(cursor);
    record->pos.z = M_GetS32(cursor);
    record->rot = M_GetXYZ16(cursor);
}

static void M_PutFlip(
    uint8_t **const cursor, const COOP_FLIP_RECORD *const record)
{
    M_PutS16(cursor, (int16_t)record->groups);
    M_PutS16(cursor, (int16_t)record->one_shot_slots);
    for (int32_t i = 0; i < MAX_FLIP_MAPS; i++) {
        M_PutU8(cursor, record->slot_masks[i]);
    }
}

static void M_GetFlip(const uint8_t **const cursor, COOP_FLIP_RECORD *const record)
{
    record->groups = (uint16_t)M_GetS16(cursor);
    record->one_shot_slots = (uint16_t)M_GetS16(cursor);
    for (int32_t i = 0; i < MAX_FLIP_MAPS; i++) {
        record->slot_masks[i] = M_GetU8(cursor);
    }
}

static uint16_t M_GetPort(void)
{
    const int32_t port = Shell_GetArgs()->coop.port;
    return port > 0 ? (uint16_t)port : COOP_DEFAULT_PORT;
}

static bool M_StartHost(const uint16_t port)
{
    const ENetAddress address = { .host = ENET_HOST_ANY, .port = port };
    m_Net.host = enet_host_create(&address, M_MAX_PEERS, M_CHANNEL_COUNT, 0, 0);
    if (m_Net.host == nullptr) {
        Console_Info("Coop: cannot host on port %d", port);
        return false;
    }
    Console_Info("Coop: hosting on port %d", port);
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
    Console_Info("Coop: joining %s:%d", target, port);
    return true;
}

// Ask the host to let this client in. The answer comes as a connect event,
// or as a disconnect one if the host cannot be reached.
static void M_Connect(void)
{
    m_Net.peer = enet_host_connect(
        m_Net.host, &m_Net.host_address, M_CHANNEL_COUNT, M_PROTOCOL_VERSION);
    if (m_Net.peer != nullptr) {
        enet_peer_timeout(
            m_Net.peer, 0, M_PEER_TIMEOUT_MIN_MS, M_PEER_TIMEOUT_MAX_MS);
    }
    m_Net.reconnect_wait = M_RECONNECT_FRAMES;
}

static bool M_Start(void)
{
    if (enet_initialize() != 0) {
        LOG_ERROR("Coop: ENet failed to start");
        return false;
    }
    m_Net.running = true;

    const SHELL_ARGS *const args = Shell_GetArgs();
    const uint16_t port = M_GetPort();
    if (args->coop.host) {
        return M_StartHost(port);
    }
    return M_StartClient(args->coop.join, port);
}

static bool M_IsHost(void)
{
    return Shell_GetArgs()->coop.host;
}

static bool M_IsConnected(void)
{
    return m_Net.peer != nullptr
        && m_Net.peer->state == ENET_PEER_STATE_CONNECTED;
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

static void M_ForgetRemote(void)
{
    m_Net.has_remote_state = false;
    m_Net.world_synced = false;
    Coop_Puppet_Remove(M_REMOTE_SLOT);
}

static void M_SendWorld(
    const int32_t level_num, const VECTOR *const items,
    const COOP_FLIP_RECORD *const flip, const bool is_all)
{
    if (!M_IsConnected() || (items->count == 0 && flip == nullptr)) {
        return;
    }

    const size_t size = M_WORLD_HEADER_SIZE + (flip != nullptr ? M_FLIP_SIZE : 0)
        + items->count * M_ITEM_RECORD_SIZE;
    ENetPacket *const packet =
        enet_packet_create(nullptr, size, ENET_PACKET_FLAG_RELIABLE);
    uint8_t *cursor = packet->data;
    M_PutU8(&cursor, M_PROTOCOL_VERSION);
    M_PutU8(&cursor, M_PACKET_WORLD);
    M_PutS32(&cursor, level_num);
    M_PutU8(&cursor, is_all ? 1 : 0);
    M_PutU8(&cursor, flip != nullptr ? 1 : 0);
    M_PutS16(&cursor, (int16_t)items->count);
    if (flip != nullptr) {
        M_PutFlip(&cursor, flip);
    }
    for (int32_t i = 0; i < items->count; i++) {
        M_PutItemRecord(&cursor, Vector_Get(items, i));
    }
    ASSERT(cursor == packet->data + size);
    enet_peer_send(m_Net.peer, M_CHANNEL_WORLD, packet);
}

static void M_SendWorldRequest(const int32_t level_num)
{
    if (!M_IsConnected()) {
        return;
    }
    uint8_t data[M_WORLD_REQUEST_SIZE];
    uint8_t *cursor = data;
    M_PutU8(&cursor, M_PROTOCOL_VERSION);
    M_PutU8(&cursor, M_PACKET_WORLD_REQUEST);
    M_PutS32(&cursor, level_num);
    ENetPacket *const packet =
        enet_packet_create(data, sizeof(data), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(m_Net.peer, M_CHANNEL_WORLD, packet);
}

static void M_SendAllOfWorld(const int32_t level_num)
{
    COOP_FLIP_RECORD flip;
    Coop_World_GetAll(m_Net.world_changes, &flip);
    M_SendWorld(level_num, m_Net.world_changes, &flip, true);
    Vector_Clear(m_Net.world_changes);
}

static void M_ReceivePlayerState(
    const uint8_t *cursor, const size_t length)
{
    if (length != M_PLAYER_STATE_SIZE) {
        return;
    }
    m_Net.remote_level_num = M_GetS32(&cursor);
    M_GetState(&cursor, &m_Net.remote_state);
    m_Net.has_remote_state = true;
}

static void M_ReceiveWorld(const uint8_t *cursor, const size_t length)
{
    if (length < M_WORLD_HEADER_SIZE) {
        return;
    }
    const int32_t level_num = M_GetS32(&cursor);
    const bool is_all = M_GetU8(&cursor) != 0;
    const bool has_flip = M_GetU8(&cursor) != 0;
    const int32_t count = (uint16_t)M_GetS16(&cursor);
    const size_t expected = M_WORLD_HEADER_SIZE
        + (has_flip ? M_FLIP_SIZE : 0) + count * M_ITEM_RECORD_SIZE;
    // A level this game is not in has nothing to take from it.
    if (length != expected || level_num != M_GetLevelNum()) {
        return;
    }

    // The host's word is final. A client's stands unless the host changed the
    // same thing itself a moment ago.
    const bool authoritative = !M_IsHost();
    if (has_flip) {
        COOP_FLIP_RECORD flip;
        M_GetFlip(&cursor, &flip);
        Coop_World_ApplyFlip(&flip, authoritative);
    }
    for (int32_t i = 0; i < count; i++) {
        COOP_ITEM_RECORD record;
        M_GetItemRecord(&cursor, &record);
        Coop_World_ApplyItem(&record, authoritative);
    }

    if (is_all && !M_IsHost()) {
        m_Net.world_synced = true;
        Console_Info("Coop: caught up with the host's level");
    }
}

static void M_ReceiveWorldRequest(const uint8_t *cursor, const size_t length)
{
    if (length != M_WORLD_REQUEST_SIZE || !M_IsHost()) {
        return;
    }
    const int32_t level_num = M_GetS32(&cursor);
    if (level_num == M_GetLevelNum()) {
        M_SendAllOfWorld(level_num);
    }
}

static void M_OnConnect(ENetPeer *const peer, const uint32_t data)
{
    // A client hears back from the host it asked for. The host checks who is
    // asking: the version rides in the connect request.
    if (Shell_GetArgs()->coop.host) {
        if (data != M_PROTOCOL_VERSION) {
            LOG_WARNING(
                "Coop: turned away a player on protocol %u (ours is %d)", data,
                M_PROTOCOL_VERSION);
            enet_peer_disconnect_now(peer, 0);
            return;
        }
        m_Net.peer = peer;
    }
    enet_peer_timeout(peer, 0, M_PEER_TIMEOUT_MIN_MS, M_PEER_TIMEOUT_MAX_MS);
    m_Net.connected = true;
    Console_Info("Coop: connected");
}

static void M_OnDisconnect(ENetPeer *const peer)
{
    if (peer != m_Net.peer) {
        return;
    }
    // A client that never got through has nobody to miss.
    const bool was_connected = m_Net.connected;
    m_Net.connected = false;
    m_Net.peer = nullptr;
    M_ForgetRemote();
    // The host waits for the next player; a client tries its host again.
    if (was_connected) {
        Console_Info(
            M_IsHost() ? "Coop: the other player left"
                       : "Coop: lost the host, trying again");
    }
}

static void M_OnReceive(const ENetPacket *const packet)
{
    if (packet->dataLength < 2) {
        return;
    }
    const uint8_t *cursor = packet->data;
    if (M_GetU8(&cursor) != M_PROTOCOL_VERSION) {
        return;
    }
    // Each reader checks the length it was handed against the length its
    // packet must have, the two leading bytes included.
    const uint8_t type = M_GetU8(&cursor);
    switch (type) {
    case M_PACKET_PLAYER_STATE:
        M_ReceivePlayerState(cursor, packet->dataLength);
        break;
    case M_PACKET_WORLD:
        M_ReceiveWorld(cursor, packet->dataLength);
        break;
    case M_PACKET_WORLD_REQUEST:
        M_ReceiveWorldRequest(cursor, packet->dataLength);
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
            M_OnReceive(event.packet);
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

static void M_SendState(const int32_t level_num)
{
    if (m_Net.peer == nullptr || m_Net.peer->state != ENET_PEER_STATE_CONNECTED) {
        return;
    }

    uint8_t data[M_PLAYER_STATE_SIZE];
    uint8_t *cursor = data;
    const COOP_PLAYER_STATE state = Coop_Puppet_CaptureLara();
    M_PutU8(&cursor, M_PROTOCOL_VERSION);
    M_PutU8(&cursor, M_PACKET_PLAYER_STATE);
    M_PutS32(&cursor, level_num);
    M_PutState(&cursor, &state);
    ASSERT(cursor == data + sizeof(data));

    // Unreliable and sequenced: a lost state is replaced by the next frame's,
    // and one that arrives late is dropped rather than played out of order.
    ENetPacket *const packet = enet_packet_create(data, sizeof(data), 0);
    enet_peer_send(m_Net.peer, M_CHANNEL_STATE, packet);
}

static void M_SyncWorld(const int32_t level_num)
{
    if (m_Net.world_changes == nullptr) {
        m_Net.world_changes = Vector_Create(sizeof(COOP_ITEM_RECORD));
    }

    // Taken every frame, connected or not, so that what is sent later is only
    // what changed since then.
    COOP_FLIP_RECORD flip;
    const bool flip_changed = Coop_World_TakeChanges(m_Net.world_changes, &flip);
    const bool same_level =
        m_Net.has_remote_state && m_Net.remote_level_num == level_num;
    if (!M_IsConnected() || !same_level) {
        return;
    }

    // A client catches up with the host's level before it has anything of its
    // own worth telling.
    if (!M_IsHost() && !m_Net.world_synced) {
        if (m_Net.world_request_wait-- <= 0) {
            M_SendWorldRequest(level_num);
            m_Net.world_request_wait = M_WORLD_REQUEST_FRAMES;
        }
        return;
    }

    M_SendWorld(
        level_num, m_Net.world_changes, flip_changed ? &flip : nullptr, false);
}

static void M_Shutdown(void);

bool Coop_Net_IsEnabled(void)
{
    const SHELL_ARGS *const args = Shell_GetArgs();
    return args->coop.host || args->coop.join != nullptr;
}

void Coop_Net_Service(void)
{
    if (!m_Net.started) {
        if (!M_Start()) {
            // Give back whatever got set up, and do not try again until the
            // next session.
            M_Shutdown();
            Console_Info("Coop: the network is off for this session");
        }
        m_Net.started = true;
    }
    if (m_Net.host == nullptr) {
        return;
    }

    M_Service();
    if (!M_IsHost() && m_Net.peer == nullptr && m_Net.reconnect_wait-- <= 0) {
        M_Connect();
    }
    enet_host_flush(m_Net.host);
}

void Coop_Net_Control(void)
{
    if (m_Net.host == nullptr) {
        return;
    }

    const GF_LEVEL *const level = GF_GetCurrentLevel();
    if (level == nullptr) {
        return;
    }
    M_SendState(level->num);
    M_SyncWorld(level->num);

    // The other player only appears while in the same level.
    if (m_Net.has_remote_state && m_Net.remote_level_num == level->num) {
        Coop_Puppet_Apply(M_REMOTE_SLOT, &m_Net.remote_state);
    } else {
        Coop_Puppet_Remove(M_REMOTE_SLOT);
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

static void M_Shutdown(void)
{
    if (m_Net.world_changes != nullptr) {
        Vector_Free(m_Net.world_changes);
    }
    if (m_Net.host != nullptr) {
        if (m_Net.peer != nullptr) {
            enet_peer_disconnect_now(m_Net.peer, 0);
        }
        enet_host_destroy(m_Net.host);
    }
    if (m_Net.running) {
        enet_deinitialize();
    }
    m_Net = (typeof(m_Net)) {};
}

REGISTER_SUBSYSTEM(.shutdown = M_Shutdown)
