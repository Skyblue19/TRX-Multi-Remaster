#pragma once

#include <trx/core/math/types.h>
#include <trx/game/lara/enum.h>

#include <stdbool.h>
#include <stdint.h>

// The puppets stand in for the other players: Laras that move only as their
// player's state says, with no simulation and no collision of their own.

#define COOP_MAX_PUPPETS 4

// An arm as the gun code poses it. Its frames are named by their index among
// the level's frames, which every copy of the level lays out alike.
typedef struct {
    int32_t frame_base; // -1 for none
    int16_t frame_num;
    int16_t anim_num;
    XYZ_16 rot;
    int16_t flash_gun;
} COOP_ARM_STATE;

// A mesh worn over a body part, a gun in a hand or a holster. Meshes are named
// by their index among the level's meshes.
typedef struct {
    bool visible;
    int32_t mesh; // -1 for none
    XYZ_16 offset;
} COOP_EQUIPMENT_STATE;

// One logic frame of a player, as much as a puppet needs to stand where the
// player stood, strike the same pose and hold the same gun the same way. It is
// what each player sends.
typedef struct {
    XYZ_32 pos;
    XYZ_16 rot;
    int16_t room_num;
    // Relative to Lara's own animations, so it holds for any object that
    // borrows them.
    int16_t anim_num;
    int16_t frame_num;

    int16_t gun_status;
    int16_t gun_type;
    XYZ_16 head_rot;
    XYZ_16 torso_rot;
    COOP_ARM_STATE left_arm;
    COOP_ARM_STATE right_arm;
    bool flare_control;
    int32_t meshes[LM_NUMBER_OF]; // -1 for the body's own
    COOP_EQUIPMENT_STATE equipment[LM_NUMBER_OF];
} COOP_PLAYER_STATE;

// Lara's state this frame. Lara must be in the level.
COOP_PLAYER_STATE Coop_Puppet_CaptureLara(void);

// Put puppet `slot` in `state`, bringing it in first if it is not in the
// level. A state that does not fit this level, as one sent from another level
// or a damaged one, is refused and leaves the puppet as it was. A look that
// does not fit is dropped for the plain one, and the puppet still moves.
bool Coop_Puppet_Apply(int32_t slot, const COOP_PLAYER_STATE *state);

// Take puppet `slot` out of the level, if it is in.
void Coop_Puppet_Remove(int32_t slot);

// Forget every puppet. The level's items and minted objects are about to go.
void Coop_Puppet_Reset(void);

// Whether an item is a puppet, which no savegame should carry.
bool Coop_Puppet_IsItem(int16_t item_num);
