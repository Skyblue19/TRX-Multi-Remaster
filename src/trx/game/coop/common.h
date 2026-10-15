#pragma once

#include <stdbool.h>
#include <stdint.h>

// What the rest of the game knows of co-op: one call a frame, one reset a
// level, and a few questions.

// Whether this session plays co-op in any form: over the network, or with the
// local ghost.
bool Coop_IsActive(void);

// Whether the current level is one a player plays. The demos replay recorded
// input against a world that must stay as recorded, and the title and
// cutscenes have nobody to show.
bool Coop_IsPlayableLevel(void);

// Called once per frame in every phase: menus and loading screens included,
// so the connection keeps going while a player is out of the level.
void Coop_Tick(void);

// Called once per logic frame of play, after Lara has moved.
void Coop_Control(void);

// Forget everything tied to the level. Its items and minted objects are about
// to go.
void Coop_Reset(void);

// Whether an item stands in for another player, which no savegame should
// carry.
bool Coop_IsPuppetItem(int16_t item_num);
