#pragma once

#include <stdbool.h>

// Co-op over the network, between two players for now: one hosts with
// --coop-host, the other joins with --coop-join. Each sends its Lara's state
// every logic frame, and shows the other's as a puppet when both are in the
// same level. Nothing in the world is shared yet.

#define COOP_DEFAULT_PORT 41300

// Whether this session plays over the network.
bool Coop_Net_IsEnabled(void);

// Starts the network on the first call. Then, each logic frame: take in what
// has arrived, send Lara's state, and move the other player's puppet.
void Coop_Net_Control(void);
