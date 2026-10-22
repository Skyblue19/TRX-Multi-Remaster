#pragma once

#include <trx/game/game_flow/types.h>
#include <trx/game/phase/types.h>

// A co-op client that loaded a level before its host waits on the level's
// loading picture until the host's level, loaded too, has told it how things
// stand. Played before then, what it did would be lost.
PHASE *Phase_CoopWait_Create(const GF_LEVEL *level);
void Phase_CoopWait_Destroy(PHASE *phase);
