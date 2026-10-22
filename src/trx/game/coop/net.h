#pragma once

#include <trx/core/json/util/read_io.h>
#include <trx/core/json/util/write_io.h>
#include <trx/core/result.h>
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
// may be loading it afresh from a save. A level that is not `fresh`, as one
// loaded from a save, is joined without its opening.
void Coop_Net_OnLevelSequence(int32_t level_num, bool fresh);
// Whether a client is in a level that has yet to catch up with the host's.
bool Coop_Net_IsWaitingForWorld(void);
// Called every frame a client waits for that: asks the host for its level
// until it comes. Returns false once there is nothing more to wait for.
bool Coop_Net_TickWaitForWorld(void);

// Whether a client is on its way into a level the host has under way, which
// it joins without the level's opening.
bool Coop_Net_SkipsIntro(void);

// The game went back to the title, which leaves the session: a client sends
// its last profile and goes, a host tells everyone the game is over.
void Coop_Net_OnTitle(void);

// The level is about to go: a client sends the host what Lara carries out of
// it.
void Coop_Net_OnLevelEnding(void);

// The players' profiles in the host's save: written into the savegame being
// made, and read back from the one being loaded, to hand back to each player
// as they come into the level.
void Coop_Net_DumpProfiles(JSON_WRITE_IO *io);
RESULT Coop_Net_LoadProfiles(JSON_READ_IO *io);

// Whether a dead Lara here should wait rather than see the death screen: on
// a client always, the host deciding what comes next; on the host while
// another player in the level lives to revive it.
bool Coop_Net_ShouldHoldDeath(void);

// The other players down in this level, up to COOP_MAX_PLAYERS of them into
// `out`; returns how many.
int32_t Coop_Net_GetDeadTeammates(COOP_PLAYER_INFO *out);

// Brings the player numbered `number` back, at full health at Lara's side,
// once the host agrees it is down. Returns false, sending nothing, where it
// is not down in this level or Lara is down herself.
bool Coop_Net_Revive(int32_t number);

// Lara reached the level's exit: a client tells the host, and the host counts
// itself in. Either then sees the level's statistics while the others finish.
void Coop_Net_OnReachExit(void);
// Whether the statistics at the level's end must stay up, the player having
// seen them: on a client until the host says where to go next, on the host
// until every living player is at the exit.
bool Coop_Net_HoldsLevelStats(void);
// Called every frame of those statistics: on the host, the level goes on
// behind them for the players still in it.
void Coop_Net_TickLevelStats(void);

// A client's shot landing on one of the host's creatures, sent to the host to
// apply. Returns false, sending nothing, where there is no host to send to.
bool Coop_Net_SendHit(int16_t item_num, int16_t damage, uint8_t flags);
// A blow puppet `index` took on the host, sent to its player to suffer.
void Coop_Net_SendDamage(int32_t index, int16_t damage, uint8_t flags);
// A client's trigger waking a creature, sent to the host, whose creature it
// is. Returns false where there is no host to send to.
bool Coop_Net_SendActivate(int16_t item_num, bool force);

// Whether a client is on its way to the level the host went to. A screen in
// front holds the move back until it closes.
bool Coop_Net_IsStartPending(void);
