#pragma once

#include <trx/core/math/types.h>
#include <trx/game/lara/types.h>

bool Lara_Cheat_GiveGun(LARA_GUN_TYPE gun_type, bool ignore_exclusions);
void Lara_Cheat_GetStuff(void);
bool Lara_Cheat_OpenNearestDoor(void);
bool Lara_Cheat_EnterFlyMode(void);
bool Lara_Cheat_ExitFlyMode(void);
bool Lara_Cheat_Teleport(XYZ_32 pos, int16_t room_num);

// Brings a dead Lara back at full health, where `pos` is, or where she
// lies if there is no room for her there. Returns false where she
// cannot come back, as after the explosion cheat.
bool Lara_Cheat_Revive(XYZ_32 pos, int16_t room_num);
