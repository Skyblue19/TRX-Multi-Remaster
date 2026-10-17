#pragma once

#include <trx/game/coop/creatures.h>
#include <trx/game/coop/puppet.h>
#include <trx/game/coop/world.h>

#include <stdint.h>

// How co-op data is written into packets: a field at a time, little end
// first, so that what goes on the wire does not hang on how either machine
// lays out a struct. Each Put moves the cursor past what it wrote and each Get
// past what it read; the caller checks a packet's length before reading it.

#define COOP_WIRE_XYZ_16_SIZE (3 * 2)
// frame base, frame, anim, rot, flash
#define COOP_WIRE_ARM_SIZE (4 + 2 + 2 + COOP_WIRE_XYZ_16_SIZE + 2)
// visible, mesh, offset
#define COOP_WIRE_EQUIPMENT_SIZE (1 + 4 + COOP_WIRE_XYZ_16_SIZE)
// The body's pos, rot, room, anim, frame and hit points; the gun's status and
// type, head and torso rots, both arms and the flare; and what is worn on
// each part.
#define COOP_WIRE_STATE_SIZE                                                   \
    (3 * 4 + COOP_WIRE_XYZ_16_SIZE + 2 + 2 + 2 + 2 + 2 + 2                     \
     + 2 * COOP_WIRE_XYZ_16_SIZE + 2 * COOP_WIRE_ARM_SIZE + 1                  \
     + LM_NUMBER_OF * (4 + COOP_WIRE_EQUIPMENT_SIZE))
// item, object, flags, trigger mask, three anim states, anim, frame, timer,
// room, pos and rot
#define COOP_WIRE_ITEM_RECORD_SIZE                                             \
    (2 + 2 + 2 + 1 + 3 * 2 + 2 + 2 + 2 + 2 + 3 * 4 + COOP_WIRE_XYZ_16_SIZE)
// item, object, flags, two anim states, anim, frame, hit points, room, pos,
// rot, meshes and four joints
#define COOP_WIRE_CREATURE_RECORD_SIZE                                         \
    (2 + 2 + 1 + 2 * 2 + 2 + 2 + 2 + 2 + 3 * 4 + COOP_WIRE_XYZ_16_SIZE + 4     \
     + 4 * 2)
// object, room, pos, rot, speed, fall speed, frame, counter, shade, two flags
#define COOP_WIRE_EFFECT_RECORD_SIZE                                           \
    (2 + 2 + 3 * 4 + COOP_WIRE_XYZ_16_SIZE + 5 * 2 + 2 * 4)
// slot, flags, item, whether placed, pos
#define COOP_WIRE_SOUND_RECORD_SIZE (4 + 4 + 2 + 1 + 3 * 4)
// groups, one-shot slots, a mask per slot
#define COOP_WIRE_FLIP_SIZE (2 + 2 + MAX_FLIP_MAPS)

void Coop_Wire_PutU8(uint8_t **cursor, uint8_t value);
void Coop_Wire_PutS16(uint8_t **cursor, int16_t value);
void Coop_Wire_PutS32(uint8_t **cursor, int32_t value);
uint8_t Coop_Wire_GetU8(const uint8_t **cursor);
int16_t Coop_Wire_GetS16(const uint8_t **cursor);
int32_t Coop_Wire_GetS32(const uint8_t **cursor);

void Coop_Wire_PutXYZ16(uint8_t **cursor, XYZ_16 value);
XYZ_16 Coop_Wire_GetXYZ16(const uint8_t **cursor);

void Coop_Wire_PutState(uint8_t **cursor, const COOP_PLAYER_STATE *state);
void Coop_Wire_GetState(const uint8_t **cursor, COOP_PLAYER_STATE *state);

void Coop_Wire_PutItemRecord(
    uint8_t **cursor, const COOP_ITEM_RECORD *record);
void Coop_Wire_GetItemRecord(const uint8_t **cursor, COOP_ITEM_RECORD *record);

void Coop_Wire_PutCreatureRecord(
    uint8_t **cursor, const COOP_CREATURE_RECORD *record);
void Coop_Wire_GetCreatureRecord(
    const uint8_t **cursor, COOP_CREATURE_RECORD *record);

void Coop_Wire_PutEffectRecord(
    uint8_t **cursor, const COOP_EFFECT_RECORD *record);
void Coop_Wire_GetEffectRecord(
    const uint8_t **cursor, COOP_EFFECT_RECORD *record);
void Coop_Wire_PutSoundRecord(
    uint8_t **cursor, const COOP_SOUND_RECORD *record);
void Coop_Wire_GetSoundRecord(
    const uint8_t **cursor, COOP_SOUND_RECORD *record);

void Coop_Wire_PutFlip(uint8_t **cursor, const COOP_FLIP_RECORD *record);
void Coop_Wire_GetFlip(const uint8_t **cursor, COOP_FLIP_RECORD *record);
