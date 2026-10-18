#pragma once

#include <trx/core/math/types.h>
#include <trx/core/vector.h>
#include <trx/game/rooms/const.h>

#include <stdbool.h>
#include <stdint.h>

// The shared world: what each game has done to the level that the others must
// see. Doors, switches, traps and pickups are kept alike through their state,
// and so are the flipped rooms. Creatures, Lara and the puppets are not.
//
// Only the level's own items take part. Their slots are never handed out
// again, so an item number names the same item in every copy of the level.

// An item's state, as far as the others need it. What sets it apart from the
// last one sent is its discrete part: the flags, trigger and anim states. The
// rest rides along so the item lands where it stood.
typedef struct {
    int16_t item_num;
    int16_t object_id;
    uint16_t flags;
    uint8_t trigger_mask;
    int16_t current_anim_state;
    int16_t goal_anim_state;
    int16_t required_anim_state;
    int16_t anim_num;
    int16_t frame_num;
    int16_t timer;
    int16_t room_num;
    XYZ_32 pos;
    XYZ_16 rot;
} COOP_ITEM_RECORD;

// Which flip groups are flipped, and the trigger state of each flip slot.
typedef struct {
    uint16_t groups;
    uint16_t one_shot_slots;
    uint8_t slot_masks[MAX_FLIP_MAPS];
} COOP_FLIP_RECORD;

// Forget the level. Called as it unloads.
void Coop_World_Reset(void);

// Take what changed in the level since the last call, as records into `items`
// (cleared first), and the flips into `flip`, returning whether they changed.
// The first call in a level takes nothing: it only learns the level as it is.
bool Coop_World_TakeChanges(VECTOR *items, COOP_FLIP_RECORD *flip);

// The blocks this game is moving now, as records into `items` (cleared first),
// to be shown moving in the other games every frame.
void Coop_World_TakeMoving(VECTOR *items);
// Show a block another game is moving where it is now. Returns whether the
// record was taken, which is what the host passes on.
bool Coop_World_ApplyMoving(const COOP_ITEM_RECORD *record);

// The whole level, for a player who has to catch up with it.
void Coop_World_GetAll(VECTOR *items, COOP_FLIP_RECORD *flip);

// Make the level match what another game reports. With `authoritative` the
// report wins outright, as the host's does. Without it, an item this game
// changed itself a moment ago keeps its own state, apart from a pickup taken,
// which is gone for everyone. A record that does not fit this level is
// skipped. Returns whether the record was taken, which is what the host
// passes on to the other games.
bool Coop_World_ApplyItem(const COOP_ITEM_RECORD *record, bool authoritative);
bool Coop_World_ApplyFlip(const COOP_FLIP_RECORD *record, bool authoritative);
