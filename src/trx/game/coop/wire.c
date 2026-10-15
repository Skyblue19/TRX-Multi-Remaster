#include <trx/game/coop/wire.h>

#include <trx/game/lara/enum.h>

void Coop_Wire_PutU8(uint8_t **const cursor, const uint8_t value)
{
    *(*cursor)++ = value;
}

void Coop_Wire_PutS16(uint8_t **const cursor, const int16_t value)
{
    const uint16_t bits = (uint16_t)value;
    Coop_Wire_PutU8(cursor, bits & 0xFF);
    Coop_Wire_PutU8(cursor, bits >> 8);
}

void Coop_Wire_PutS32(uint8_t **const cursor, const int32_t value)
{
    const uint32_t bits = (uint32_t)value;
    for (int32_t i = 0; i < 4; i++) {
        Coop_Wire_PutU8(cursor, (bits >> (8 * i)) & 0xFF);
    }
}

uint8_t Coop_Wire_GetU8(const uint8_t **const cursor)
{
    return *(*cursor)++;
}

int16_t Coop_Wire_GetS16(const uint8_t **const cursor)
{
    const uint16_t lo = Coop_Wire_GetU8(cursor);
    const uint16_t hi = Coop_Wire_GetU8(cursor);
    return (int16_t)(lo | (hi << 8));
}

int32_t Coop_Wire_GetS32(const uint8_t **const cursor)
{
    uint32_t bits = 0;
    for (int32_t i = 0; i < 4; i++) {
        bits |= (uint32_t)Coop_Wire_GetU8(cursor) << (8 * i);
    }
    return (int32_t)bits;
}

void Coop_Wire_PutXYZ16(uint8_t **const cursor, const XYZ_16 value)
{
    Coop_Wire_PutS16(cursor, value.x);
    Coop_Wire_PutS16(cursor, value.y);
    Coop_Wire_PutS16(cursor, value.z);
}

XYZ_16 Coop_Wire_GetXYZ16(const uint8_t **const cursor)
{
    // Read one at a time: the order of initialisers is not the order of
    // evaluation.
    const int16_t x = Coop_Wire_GetS16(cursor);
    const int16_t y = Coop_Wire_GetS16(cursor);
    const int16_t z = Coop_Wire_GetS16(cursor);
    return (XYZ_16) { .x = x, .y = y, .z = z };
}

static void M_PutArm(uint8_t **const cursor, const COOP_ARM_STATE *const arm)
{
    Coop_Wire_PutS32(cursor, arm->frame_base);
    Coop_Wire_PutS16(cursor, arm->frame_num);
    Coop_Wire_PutS16(cursor, arm->anim_num);
    Coop_Wire_PutXYZ16(cursor, arm->rot);
    Coop_Wire_PutS16(cursor, arm->flash_gun);
}

static void M_GetArm(const uint8_t **const cursor, COOP_ARM_STATE *const arm)
{
    arm->frame_base = Coop_Wire_GetS32(cursor);
    arm->frame_num = Coop_Wire_GetS16(cursor);
    arm->anim_num = Coop_Wire_GetS16(cursor);
    arm->rot = Coop_Wire_GetXYZ16(cursor);
    arm->flash_gun = Coop_Wire_GetS16(cursor);
}

void Coop_Wire_PutState(
    uint8_t **const cursor, const COOP_PLAYER_STATE *const state)
{
    Coop_Wire_PutS32(cursor, state->pos.x);
    Coop_Wire_PutS32(cursor, state->pos.y);
    Coop_Wire_PutS32(cursor, state->pos.z);
    Coop_Wire_PutXYZ16(cursor, state->rot);
    Coop_Wire_PutS16(cursor, state->room_num);
    Coop_Wire_PutS16(cursor, state->anim_num);
    Coop_Wire_PutS16(cursor, state->frame_num);
    Coop_Wire_PutS16(cursor, state->hit_points);
    Coop_Wire_PutS16(cursor, state->gun_status);
    Coop_Wire_PutS16(cursor, state->gun_type);
    Coop_Wire_PutXYZ16(cursor, state->head_rot);
    Coop_Wire_PutXYZ16(cursor, state->torso_rot);
    M_PutArm(cursor, &state->left_arm);
    M_PutArm(cursor, &state->right_arm);
    Coop_Wire_PutU8(cursor, state->flare_control ? 1 : 0);
    for (int32_t i = 0; i < LM_NUMBER_OF; i++) {
        Coop_Wire_PutS32(cursor, state->meshes[i]);
        const COOP_EQUIPMENT_STATE *const equipment = &state->equipment[i];
        Coop_Wire_PutU8(cursor, equipment->visible ? 1 : 0);
        Coop_Wire_PutS32(cursor, equipment->mesh);
        Coop_Wire_PutXYZ16(cursor, equipment->offset);
    }
}

void Coop_Wire_GetState(
    const uint8_t **const cursor, COOP_PLAYER_STATE *const state)
{
    state->pos.x = Coop_Wire_GetS32(cursor);
    state->pos.y = Coop_Wire_GetS32(cursor);
    state->pos.z = Coop_Wire_GetS32(cursor);
    state->rot = Coop_Wire_GetXYZ16(cursor);
    state->room_num = Coop_Wire_GetS16(cursor);
    state->anim_num = Coop_Wire_GetS16(cursor);
    state->frame_num = Coop_Wire_GetS16(cursor);
    state->hit_points = Coop_Wire_GetS16(cursor);
    state->gun_status = Coop_Wire_GetS16(cursor);
    state->gun_type = Coop_Wire_GetS16(cursor);
    state->head_rot = Coop_Wire_GetXYZ16(cursor);
    state->torso_rot = Coop_Wire_GetXYZ16(cursor);
    M_GetArm(cursor, &state->left_arm);
    M_GetArm(cursor, &state->right_arm);
    state->flare_control = Coop_Wire_GetU8(cursor) != 0;
    for (int32_t i = 0; i < LM_NUMBER_OF; i++) {
        state->meshes[i] = Coop_Wire_GetS32(cursor);
        COOP_EQUIPMENT_STATE *const equipment = &state->equipment[i];
        equipment->visible = Coop_Wire_GetU8(cursor) != 0;
        equipment->mesh = Coop_Wire_GetS32(cursor);
        equipment->offset = Coop_Wire_GetXYZ16(cursor);
    }
}

void Coop_Wire_PutItemRecord(
    uint8_t **const cursor, const COOP_ITEM_RECORD *const record)
{
    Coop_Wire_PutS16(cursor, record->item_num);
    Coop_Wire_PutS16(cursor, record->object_id);
    Coop_Wire_PutS16(cursor, (int16_t)record->flags);
    Coop_Wire_PutU8(cursor, record->trigger_mask);
    Coop_Wire_PutS16(cursor, record->current_anim_state);
    Coop_Wire_PutS16(cursor, record->goal_anim_state);
    Coop_Wire_PutS16(cursor, record->required_anim_state);
    Coop_Wire_PutS16(cursor, record->anim_num);
    Coop_Wire_PutS16(cursor, record->frame_num);
    Coop_Wire_PutS16(cursor, record->timer);
    Coop_Wire_PutS16(cursor, record->room_num);
    Coop_Wire_PutS32(cursor, record->pos.x);
    Coop_Wire_PutS32(cursor, record->pos.y);
    Coop_Wire_PutS32(cursor, record->pos.z);
    Coop_Wire_PutXYZ16(cursor, record->rot);
}

void Coop_Wire_GetItemRecord(
    const uint8_t **const cursor, COOP_ITEM_RECORD *const record)
{
    record->item_num = Coop_Wire_GetS16(cursor);
    record->object_id = Coop_Wire_GetS16(cursor);
    record->flags = (uint16_t)Coop_Wire_GetS16(cursor);
    record->trigger_mask = Coop_Wire_GetU8(cursor);
    record->current_anim_state = Coop_Wire_GetS16(cursor);
    record->goal_anim_state = Coop_Wire_GetS16(cursor);
    record->required_anim_state = Coop_Wire_GetS16(cursor);
    record->anim_num = Coop_Wire_GetS16(cursor);
    record->frame_num = Coop_Wire_GetS16(cursor);
    record->timer = Coop_Wire_GetS16(cursor);
    record->room_num = Coop_Wire_GetS16(cursor);
    record->pos.x = Coop_Wire_GetS32(cursor);
    record->pos.y = Coop_Wire_GetS32(cursor);
    record->pos.z = Coop_Wire_GetS32(cursor);
    record->rot = Coop_Wire_GetXYZ16(cursor);
}

void Coop_Wire_PutFlip(
    uint8_t **const cursor, const COOP_FLIP_RECORD *const record)
{
    Coop_Wire_PutS16(cursor, (int16_t)record->groups);
    Coop_Wire_PutS16(cursor, (int16_t)record->one_shot_slots);
    for (int32_t i = 0; i < MAX_FLIP_MAPS; i++) {
        Coop_Wire_PutU8(cursor, record->slot_masks[i]);
    }
}

void Coop_Wire_GetFlip(const uint8_t **const cursor, COOP_FLIP_RECORD *const record)
{
    record->groups = (uint16_t)Coop_Wire_GetS16(cursor);
    record->one_shot_slots = (uint16_t)Coop_Wire_GetS16(cursor);
    for (int32_t i = 0; i < MAX_FLIP_MAPS; i++) {
        record->slot_masks[i] = Coop_Wire_GetU8(cursor);
    }
}

