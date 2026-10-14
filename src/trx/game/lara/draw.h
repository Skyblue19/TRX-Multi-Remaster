#pragma once

#include <trx/game/items/types.h>
#include <trx/game/lara/skin/types.h>
#include <trx/game/lara/types.h>

// How a Lara looks beyond her body's animation: the gun she holds, how her
// arms, torso and head turn to aim it, and the meshes she wears. It is what
// another Lara needs to be drawn as her player sees her.
typedef struct {
    LARA_GUN_STATE gun_status;
    LARA_GUN_TYPE gun_type;
    XYZ_16 head_rot;
    XYZ_16 torso_rot;
    LARA_ARM left_arm;
    LARA_ARM right_arm;
    bool flare_control;
    OBJECT_MESH *meshes[LM_NUMBER_OF];
    LARA_SKIN_EQUIPMENT equipment[LM_NUMBER_OF];
} LARA_APPEARANCE;

bool Lara_Draw(const ITEM *item);

// How Lara looks this frame.
LARA_APPEARANCE Lara_GetAppearance(void);

// Draw another item, built on Lara's object, as a Lara with this appearance.
// Lara's own state is the same afterwards as before.
bool Lara_DrawAppearance(const ITEM *item, const LARA_APPEARANCE *appearance);
