#pragma once

#include <stdbool.h>
#include <stdint.h>

// What the rest of the game knows of co-op: one call a frame, one reset a
// level, and a few questions.

// Whether this session plays co-op in any form: over the network, or with the
// local ghost.
bool Coop_IsActive(void);

// Called once per logic frame, after Lara has moved.
void Coop_Control(void);

// Forget everything tied to the level. Its items and minted objects are about
// to go.
void Coop_Reset(void);

// Whether an item stands in for another player, which no savegame should
// carry.
bool Coop_IsPuppetItem(int16_t item_num);
