#pragma once

#include <stdbool.h>

// Co-op over the network, between two players for now: one hosts with
// --coop-host, the other joins with --coop-join. Each sends its Lara's state
// every logic frame, and shows the other's as a puppet when both are in the
// same level. The host's level is the one that counts: a client catches up
// with it on arrival, and both then send what they change in it.

#define COOP_DEFAULT_PORT 41300

// Whether this session plays over the network.
bool Coop_Net_IsEnabled(void);

// Called every frame whatever the game is doing. Starts the network on the
// first call, then takes in what has arrived and keeps the connection alive,
// reconnecting a client that lost its host.
void Coop_Net_Service(void);

// Called each logic frame of play: send Lara's state and what changed in the
// level, and move the other player's puppet.
void Coop_Net_Control(void);

// The level is going, so whatever comes next has to catch up with the host's
// level afresh.
void Coop_Net_OnLevelUnload(void);
