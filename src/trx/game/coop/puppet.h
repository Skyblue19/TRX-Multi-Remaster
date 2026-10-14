#pragma once

#include <trx/core/math/types.h>

#include <stdbool.h>
#include <stdint.h>

// The puppets stand in for the other players: Laras that move only as their
// player's state says, with no simulation and no collision of their own.

#define COOP_MAX_PUPPETS 4

// One logic frame of a player, as much as a puppet needs to stand where the
// player stood and strike the same pose. It is what each player sends.
typedef struct {
    XYZ_32 pos;
    XYZ_16 rot;
    int16_t room_num;
    // Relative to Lara's own animations, so it holds for any object that
    // borrows them.
    int16_t anim_num;
    int16_t frame_num;
} COOP_PLAYER_STATE;

// Lara's state this frame. Lara must be in the level.
COOP_PLAYER_STATE Coop_Puppet_CaptureLara(void);

// Put puppet `slot` in `state`, bringing it in first if it is not in the
// level. A state that does not fit this level, as one sent from another level
// or a damaged one, is refused and leaves the puppet as it was.
bool Coop_Puppet_Apply(int32_t slot, const COOP_PLAYER_STATE *state);

// Take puppet `slot` out of the level, if it is in.
void Coop_Puppet_Remove(int32_t slot);

// Forget every puppet. The level's items and minted objects are about to go.
void Coop_Puppet_Reset(void);

// Whether an item is a puppet, which no savegame should carry.
bool Coop_Puppet_IsItem(int16_t item_num);
