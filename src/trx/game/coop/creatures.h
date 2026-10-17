#pragma once

#include <trx/core/math/types.h>
#include <trx/core/vector.h>
#include <trx/game/items/enum.h>
#include <trx/game/items/types.h>
#include <trx/game/sound/ids.h>

#include <stdbool.h>
#include <stdint.h>

// Shared creatures: the host runs every creature's AI, against whichever
// player is nearest, and tells the others where each one is and what it does.
// The others only show them. A shot a client lands is the host's to apply,
// and a bite a puppet takes on the host is its player's to suffer.
//
// Like the rest of the shared world, only the level's own items take part.

// A creature as the others need to see it. What sets it apart from the last
// one sent is its discrete part: the object, flags, anim states, hit points
// and meshes. The rest moves every frame, and is sent every frame while the
// creature is simulated.
typedef struct {
    int16_t item_num;
    int16_t object_id;
    uint8_t flags;
    int16_t current_anim_state;
    int16_t goal_anim_state;
    int16_t anim_num;
    int16_t frame_num;
    int16_t hit_points;
    int16_t room_num;
    XYZ_32 pos;
    XYZ_16 rot;
    uint32_t mesh_bits;
    int16_t joints[4];
} COOP_CREATURE_RECORD;

// What a creature set off on the host as it ran: an effect, such as the blood
// of a bite, a shot's flash or a dart on its way, as it stood once the
// creature was through for the frame.
typedef struct {
    int16_t object_id;
    int16_t room_num;
    XYZ_32 pos;
    XYZ_16 rot;
    int16_t speed;
    int16_t fall_speed;
    int16_t frame_num;
    int16_t counter;
    int16_t shade;
    int32_t flag1;
    int32_t flag2;
} COOP_EFFECT_RECORD;

// And a sound, from the creature itself where it follows it about, or from a
// place.
typedef struct {
    int32_t slot;
    uint32_t flags;
    int16_t item_num; // NO_ITEM for a sound not on the creature
    bool has_pos;
    XYZ_32 pos;
} COOP_SOUND_RECORD;

// Forget the level. Called as it unloads.
void Coop_Creatures_Reset(void);

// Whether an item is one of the level's creatures, which the host runs.
bool Coop_Creatures_IsShared(int16_t item_num);

// On the host, each logic frame: the creatures simulated now into `moving`,
// and those whose discrete part changed since the last call into `changed`,
// both cleared first. The first call in a level learns it and reports no
// change.
void Coop_Creatures_Take(VECTOR *moving, VECTOR *changed);

// The host's frame count, which stamps what it sends of its creatures.
int32_t Coop_Creatures_GetFrame(void);

// Every creature in the level, for a player who has to catch up with it.
void Coop_Creatures_GetAll(VECTOR *records);

// On a client: keep a record from the host, stamped with the host's frame, to
// apply in the next logic frame. One older than the last kept is dropped, as
// a late unreliable packet would undo a reliable one, and so is any before the
// level is learnt, in play.
void Coop_Creatures_Receive(const COOP_CREATURE_RECORD *record, int32_t frame);

// On a client, each logic frame: learn the level if it is new, and put the
// creatures where the host said.
void Coop_Creatures_ApplyReceived(void);

// On the host, around each creature's control: point it at the nearest living
// player. Where that is another player, the creature runs against that
// player's puppet, which stands in for Lara meanwhile.
void Coop_Creatures_BeginControl(int16_t item_num);
void Coop_Creatures_EndControl(void);

// The puppet slot the host's stand-in is, or -1 while there is none.
int32_t Coop_Creatures_GetStandInSlot(void);

// On the host, whether Lara is out of the level at its exit, which the
// creatures stop going after. Forgotten with the level.
void Coop_Creatures_SetLaraAway(bool away);
bool Coop_Creatures_IsLaraAway(void);

// On the host, while a creature runs: the effects it makes and ends, and the
// sounds it plays, to be told to the others.
void Coop_Creatures_OnEffectCreated(int16_t effect_num);
void Coop_Creatures_OnEffectDestroyed(int16_t effect_num);
void Coop_Creatures_OnSound(
    SAMPLE_SLOT slot, const XYZ_32 *pos, uint32_t flags);
// On the host, each logic frame: what the creatures set off since the last
// call, into `effects` and `sounds`, both cleared first.
void Coop_Creatures_TakeEvents(VECTOR *effects, VECTOR *sounds);

// On a client: set off what a creature set off on the host. The effects then
// run here as anywhere, a dart striking Lara here included.
void Coop_Creatures_PlayEffect(const COOP_EFFECT_RECORD *record);
void Coop_Creatures_PlaySound(const COOP_SOUND_RECORD *record);
