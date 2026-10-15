#pragma once

#include <trx/game/coop/identity.h>

#include <stdbool.h>
#include <stdint.h>

// Co-op over the network, for up to four players: one hosts, the others join
// it. Each game sends its Lara's state every logic frame, the host passes the
// states on, and each game shows the others as puppets while they share its
// level. The host's level is the one that counts: a client catches up with it
// on arrival, and every game then sends what it changes in it.
//
// A session starts from the command line (--coop-host, --coop-join) or from
// the game's own menus, and lasts until it is left.

#define COOP_DEFAULT_PORT 41300
#define COOP_MAX_PLAYERS 4

typedef enum {
    COOP_NET_OFF,
    // Waiting for players, or playing with them.
    COOP_NET_HOSTING,
    // Trying to reach the host, or to reach it again.
    COOP_NET_CONNECTING,
    COOP_NET_CONNECTED,
} COOP_NET_STATUS;

typedef struct {
    int32_t number; // from 1, the host's being 1
    char name[COOP_NAME_SIZE];
    bool connected;
    bool is_local;
} COOP_PLAYER_INFO;

// Whether this session plays over the network.
bool Coop_Net_IsEnabled(void);
bool Coop_Net_IsHost(void);
COOP_NET_STATUS Coop_Net_GetStatus(void);

// Start hosting, or start trying to reach the host at `address`. Either ends
// any session already going. Returns false where the network could not start.
bool Coop_Net_Host(void);
bool Coop_Net_Join(const char *address);
// End the session, letting the other games know.
void Coop_Net_Leave(void);

// The players this game knows of, fills up to COOP_MAX_PLAYERS of them into
// `out` and returns how many it filled.
int32_t Coop_Net_GetPlayers(COOP_PLAYER_INFO *out);

// Called every frame whatever the game is doing. Starts the session the
// command line asks for on the first call, then takes in what has arrived and
// keeps the connection alive, reconnecting a client that lost its host.
void Coop_Net_Service(void);

// Called each logic frame of play: send Lara's state and what changed in the
// level, and move the other players' puppets.
void Coop_Net_Control(void);

// The level is going, so whatever comes next has to catch up with the host's
// level afresh.
void Coop_Net_OnLevelUnload(void);

// A playable level's sequence is starting here. On the host, the others are
// told to start it too, even when it is the level they are in, as the host
// may be loading it afresh from a save.
void Coop_Net_OnLevelSequence(int32_t level_num);

// Whether a client is on its way to the level the host went to. A screen in
// front holds the move back until it closes.
bool Coop_Net_IsStartPending(void);
