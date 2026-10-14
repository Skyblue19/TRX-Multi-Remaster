#pragma once

#include <stdbool.h>
#include <stdint.h>

// A second Lara that replays Lara's own recent moves a little behind her.
//
// It is the first step towards co-op: the remote players will be puppets
// driven by a stream of player states, and this one is fed by a local
// recording of Lara instead of the network. Enabled with --coop-ghost.

// Record Lara's state for this frame and move the ghost along. Called once per
// logic frame, after Lara has moved.
void Coop_Ghost_Control(void);

// Forget the ghost. The level's items and minted objects are about to go.
void Coop_Ghost_Reset(void);

// Whether an item is the ghost, which no savegame should carry.
bool Coop_Ghost_IsItem(int16_t item_num);
