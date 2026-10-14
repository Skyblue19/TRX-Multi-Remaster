#include <trx/game/coop/net.h>

#include <trx/core/log.h>
#include <trx/core/subsystem.h>
#include <trx/game/console.h>
#include <trx/game/coop/puppet.h>
#include <trx/game/game_flow.h>
#include <trx/game/shell.h>

#include <enet/enet.h>

#include <stdint.h>

// Bumped whenever a packet changes shape. A peer on another version is turned
// away when it connects, rather than misreading every packet after.
#define M_PROTOCOL_VERSION 1
#define M_CHANNEL_COUNT 1
#define M_CHANNEL_STATE 0
// The host takes one other player for now.
#define M_MAX_PEERS 1
// The other player's puppet.
#define M_REMOTE_SLOT 0
// The game only takes in packets while a level is being played, so a long
// stay in the inventory must not read as a dropped connection.
#define M_PEER_TIMEOUT_MIN_MS 5000
#define M_PEER_TIMEOUT_MAX_MS 60000

typedef enum {
    M_PACKET_PLAYER_STATE = 1,
} M_PACKET_TYPE;

// version, type, level, then the state: pos, rot, room, anim, frame.
#define M_PLAYER_STATE_SIZE (1 + 1 + 4 + 3 * 4 + 3 * 2 + 2 + 2 + 2)

static struct {
    bool started;
    bool running;
    ENetHost *host;
    // The other player. Set as soon as a client asks to connect, and on the
    // host once a client has.
    ENetPeer *peer;
    bool has_remote_state;
    int32_t remote_level_num;
    COOP_PLAYER_STATE remote_state;
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

    ENetAddress address = { .port = port };
    if (enet_address_set_host(&address, target) != 0) {
        Console_Info("Coop: cannot find %s", target);
        return false;
    }

    m_Net.peer = enet_host_connect(
        m_Net.host, &address, M_CHANNEL_COUNT, M_PROTOCOL_VERSION);
    if (m_Net.peer == nullptr) {
        Console_Info("Coop: cannot connect to %s", target);
        return false;
    }
    Console_Info("Coop: joining %s:%d", target, port);
    return true;
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

static void M_ForgetRemote(void)
{
    m_Net.has_remote_state = false;
    Coop_Puppet_Remove(M_REMOTE_SLOT);
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
    Console_Info("Coop: connected");
}

static void M_OnDisconnect(ENetPeer *const peer)
{
    if (peer != m_Net.peer) {
        return;
    }
    // The host waits for someone else; a client has nobody left to talk to.
    m_Net.peer = nullptr;
    M_ForgetRemote();
    Console_Info("Coop: the other player left");
}

static void M_OnReceive(const ENetPacket *const packet)
{
    if (packet->dataLength != M_PLAYER_STATE_SIZE) {
        return;
    }
    const uint8_t *cursor = packet->data;
    if (M_GetU8(&cursor) != M_PROTOCOL_VERSION
        || M_GetU8(&cursor) != M_PACKET_PLAYER_STATE) {
        return;
    }

    m_Net.remote_level_num = M_GetS32(&cursor);
    COOP_PLAYER_STATE *const state = &m_Net.remote_state;
    state->pos.x = M_GetS32(&cursor);
    state->pos.y = M_GetS32(&cursor);
    state->pos.z = M_GetS32(&cursor);
    state->rot.x = M_GetS16(&cursor);
    state->rot.y = M_GetS16(&cursor);
    state->rot.z = M_GetS16(&cursor);
    state->room_num = M_GetS16(&cursor);
    state->anim_num = M_GetS16(&cursor);
    state->frame_num = M_GetS16(&cursor);
    m_Net.has_remote_state = true;
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
    M_PutS32(&cursor, state.pos.x);
    M_PutS32(&cursor, state.pos.y);
    M_PutS32(&cursor, state.pos.z);
    M_PutS16(&cursor, state.rot.x);
    M_PutS16(&cursor, state.rot.y);
    M_PutS16(&cursor, state.rot.z);
    M_PutS16(&cursor, state.room_num);
    M_PutS16(&cursor, state.anim_num);
    M_PutS16(&cursor, state.frame_num);

    // Unreliable and sequenced: a lost state is replaced by the next frame's,
    // and one that arrives late is dropped rather than played out of order.
    ENetPacket *const packet = enet_packet_create(data, sizeof(data), 0);
    enet_peer_send(m_Net.peer, M_CHANNEL_STATE, packet);
}

static void M_Shutdown(void);

bool Coop_Net_IsEnabled(void)
{
    const SHELL_ARGS *const args = Shell_GetArgs();
    return args->coop.host || args->coop.join != nullptr;
}

void Coop_Net_Control(void)
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

    const GF_LEVEL *const level = GF_GetCurrentLevel();
    if (level == nullptr) {
        return;
    }
    M_SendState(level->num);

    // The other player only appears while in the same level.
    if (m_Net.has_remote_state && m_Net.remote_level_num == level->num) {
        Coop_Puppet_Apply(M_REMOTE_SLOT, &m_Net.remote_state);
    } else {
        Coop_Puppet_Remove(M_REMOTE_SLOT);
    }

    enet_host_flush(m_Net.host);
}

static void M_Shutdown(void)
{
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
