#include <trx/game/coop/world.h>

#include <trx/core/memory.h>
#include <trx/game/anims.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/objects.h>
#include <trx/game/rooms.h>

// How long a change of this game's own keeps it from being overruled by
// another non-host game: a second of logic frames.
#define M_RECENT_FRAMES 30
// The flipped-last status rides in the top bit of the group set.
#define M_FLIP_STATUS_BIT (1 << 15)
static_assert(MAX_FLIP_MAPS <= 15, "flip groups must fit below the status");

typedef enum {
    M_FLAG_SIMULATED = 1 << 0,
    M_FLAG_VISIBLE = 1 << 1,
    M_FLAG_FINISHED = 1 << 2,
    M_FLAG_DESTROYED = 1 << 3,
    M_FLAG_TRIGGER_REVERSED = 1 << 4,
    M_FLAG_TRIGGER_SPENT = 1 << 5,
    M_FLAG_TRIGGER_SWITCH_SPENT = 1 << 6,
    M_FLAG_TRIGGER_ANTI_SPENT = 1 << 7,
} M_FLAG;

static struct {
    // Whether the level has been learnt since it loaded.
    bool known;
    int32_t item_count;
    // What the others were last told of each item, and when this game last
    // changed it itself.
    COOP_ITEM_RECORD *sent;
    int32_t *changed_at;
    COOP_FLIP_RECORD sent_flip;
    int32_t flip_changed_at;
    int32_t frame;
} m_World;

// Creatures are the next step and Lara is her player's own, so neither is
// shared here.
static bool M_IsShared(const int16_t item_num)
{
    if (item_num < 0 || item_num >= m_World.item_count) {
        return false;
    }
    const ITEM *const item = Item_Get(item_num);
    if (item == Lara_GetItem() || item->object_id == O_LARA) {
        return false;
    }
    return !Object_Get(item->object_id)->intelligent;
}

static COOP_ITEM_RECORD M_Encode(const int16_t item_num)
{
    const ITEM *const item = Item_Get(item_num);
    uint16_t flags = 0;
    flags |= item->is_simulated ? M_FLAG_SIMULATED : 0;
    flags |= item->is_visible ? M_FLAG_VISIBLE : 0;
    flags |= item->is_finished ? M_FLAG_FINISHED : 0;
    flags |= item->is_destroyed ? M_FLAG_DESTROYED : 0;
    flags |= item->trigger.reversed ? M_FLAG_TRIGGER_REVERSED : 0;
    flags |= item->trigger.spent ? M_FLAG_TRIGGER_SPENT : 0;
    flags |= item->trigger.switch_spent ? M_FLAG_TRIGGER_SWITCH_SPENT : 0;
    flags |= item->trigger.anti_spent ? M_FLAG_TRIGGER_ANTI_SPENT : 0;
    return (COOP_ITEM_RECORD) {
        .item_num = item_num,
        .object_id = item->object_id,
        .flags = flags,
        .trigger_mask = item->trigger.mask,
        .current_anim_state = item->current_anim_state,
        .goal_anim_state = item->goal_anim_state,
        .required_anim_state = item->required_anim_state,
        .anim_num = item->anim_num,
        .frame_num = item->frame_num,
        .timer = item->timer,
        .room_num = item->room_num,
        .pos = item->pos,
        .rot = item->rot,
    };
}

// Only the discrete state counts as a change. An item mid-animation moves and
// counts down every frame, and saying so every frame would drown the rest.
static bool M_IsSameState(
    const COOP_ITEM_RECORD *const a, const COOP_ITEM_RECORD *const b)
{
    return a->object_id == b->object_id && a->flags == b->flags
        && a->trigger_mask == b->trigger_mask
        && a->current_anim_state == b->current_anim_state
        && a->goal_anim_state == b->goal_anim_state
        && a->required_anim_state == b->required_anim_state;
}

static COOP_FLIP_RECORD M_EncodeFlip(void)
{
    COOP_FLIP_RECORD record = {};
    for (int32_t i = 0; i < MAX_FLIP_MAPS; i++) {
        record.groups |= Room_GetFlipGroupStatus(i) ? (1 << i) : 0;
        const FLIP_SLOT *const slot = Room_GetFlipSlot(i);
        record.slot_masks[i] = slot->mask;
        record.one_shot_slots |= slot->is_one_shot ? (1 << i) : 0;
    }
    record.groups |= Room_GetFlipStatus() ? M_FLIP_STATUS_BIT : 0;
    return record;
}

static bool M_IsSameFlip(
    const COOP_FLIP_RECORD *const a, const COOP_FLIP_RECORD *const b)
{
    if (a->groups != b->groups || a->one_shot_slots != b->one_shot_slots) {
        return false;
    }
    for (int32_t i = 0; i < MAX_FLIP_MAPS; i++) {
        if (a->slot_masks[i] != b->slot_masks[i]) {
            return false;
        }
    }
    return true;
}

static void M_Learn(void)
{
    m_World.item_count = Item_GetLevelCount();
    m_World.sent =
        Memory_Alloc(sizeof(COOP_ITEM_RECORD) * MAX(m_World.item_count, 1));
    m_World.changed_at =
        Memory_Alloc(sizeof(int32_t) * MAX(m_World.item_count, 1));
    for (int32_t i = 0; i < m_World.item_count; i++) {
        m_World.sent[i] = M_Encode(i);
        m_World.changed_at[i] = -M_RECENT_FRAMES;
    }
    m_World.sent_flip = M_EncodeFlip();
    m_World.flip_changed_at = -M_RECENT_FRAMES;
    m_World.known = true;
}

static bool M_IsRecent(const int32_t frame)
{
    return m_World.frame - frame < M_RECENT_FRAMES;
}

static void M_ApplyAnim(ITEM *const item, const COOP_ITEM_RECORD *const record)
{
    item->current_anim_state = record->current_anim_state;
    item->goal_anim_state = record->goal_anim_state;
    item->required_anim_state = record->required_anim_state;

    if (record->anim_num < 0 || record->anim_num >= Anim_GetTotalCount()) {
        return;
    }
    const ANIM *const anim = Anim_GetAnim(record->anim_num);
    if (record->frame_num < anim->frame_base
        || record->frame_num > anim->frame_end) {
        return;
    }
    item->anim_num = record->anim_num;
    item->frame_num = record->frame_num;
}

static void M_ApplyState(
    const int16_t item_num, const COOP_ITEM_RECORD *const record)
{
    ITEM *const item = Item_Get(item_num);
    const uint16_t flags = record->flags;

    item->trigger.mask = record->trigger_mask & TRIGGER_MASK_ALL;
    item->trigger.reversed = (flags & M_FLAG_TRIGGER_REVERSED) != 0;
    item->trigger.spent = (flags & M_FLAG_TRIGGER_SPENT) != 0;
    item->trigger.switch_spent = (flags & M_FLAG_TRIGGER_SWITCH_SPENT) != 0;
    item->trigger.anti_spent = (flags & M_FLAG_TRIGGER_ANTI_SPENT) != 0;
    item->is_finished = (flags & M_FLAG_FINISHED) != 0;
    item->timer = record->timer;
    Item_SetVisible(item, (flags & M_FLAG_VISIBLE) != 0);

    const bool simulated = (flags & M_FLAG_SIMULATED) != 0;
    if (simulated && !item->is_simulated) {
        Item_AddSimulated(item_num);
    } else if (!simulated && item->is_simulated) {
        Item_RemoveSimulated(item_num);
    }

    M_ApplyAnim(item, record);

    if (record->room_num < 0 || record->room_num >= Room_GetCount()) {
        return;
    }
    if (item->is_present && item->room_num != record->room_num) {
        Item_UpdateRoom(item_num, record->room_num);
    }
    item->pos = record->pos;
    item->rot = record->rot;
}

void Coop_World_Reset(void)
{
    Memory_FreePointer(&m_World.sent);
    Memory_FreePointer(&m_World.changed_at);
    m_World = (typeof(m_World)) {};
}

bool Coop_World_TakeChanges(VECTOR *const items, COOP_FLIP_RECORD *const flip)
{
    Vector_Clear(items);
    m_World.frame++;
    if (!m_World.known) {
        M_Learn();
        return false;
    }

    for (int16_t i = 0; i < m_World.item_count; i++) {
        if (!M_IsShared(i)) {
            continue;
        }
        const COOP_ITEM_RECORD record = M_Encode(i);
        if (M_IsSameState(&record, &m_World.sent[i])) {
            continue;
        }
        m_World.sent[i] = record;
        m_World.changed_at[i] = m_World.frame;
        Vector_Add(items, &record);
    }

    const COOP_FLIP_RECORD current_flip = M_EncodeFlip();
    if (M_IsSameFlip(&current_flip, &m_World.sent_flip)) {
        return false;
    }
    m_World.sent_flip = current_flip;
    m_World.flip_changed_at = m_World.frame;
    *flip = current_flip;
    return true;
}

void Coop_World_GetAll(VECTOR *const items, COOP_FLIP_RECORD *const flip)
{
    Vector_Clear(items);
    if (!m_World.known) {
        M_Learn();
    }
    for (int16_t i = 0; i < m_World.item_count; i++) {
        if (!M_IsShared(i)) {
            continue;
        }
        const COOP_ITEM_RECORD record = M_Encode(i);
        Vector_Add(items, &record);
    }
    *flip = M_EncodeFlip();
}

void Coop_World_ApplyItem(
    const COOP_ITEM_RECORD *const record, const bool authoritative)
{
    if (!m_World.known) {
        M_Learn();
    }
    const int16_t item_num = record->item_num;
    if (!M_IsShared(item_num)) {
        return;
    }
    ITEM *const item = Item_Get(item_num);
    if (item->object_id != record->object_id || item->is_destroyed) {
        return;
    }

    // A pickup taken is gone for everyone, whoever saw it last.
    if ((record->flags & M_FLAG_DESTROYED) != 0) {
        Item_Destroy(item_num);
    } else if (authoritative || !M_IsRecent(m_World.changed_at[item_num])) {
        M_ApplyState(item_num, record);
    } else {
        return;
    }
    // What came in needs no echo back.
    m_World.sent[item_num] = M_Encode(item_num);
}

void Coop_World_ApplyFlip(
    const COOP_FLIP_RECORD *const record, const bool authoritative)
{
    if (!m_World.known) {
        M_Learn();
    }
    if (!authoritative && M_IsRecent(m_World.flip_changed_at)) {
        return;
    }

    for (int32_t i = 0; i < MAX_FLIP_MAPS; i++) {
        const bool flipped = (record->groups & (1 << i)) != 0;
        if (flipped != Room_GetFlipGroupStatus(i)) {
            Room_FlipMap(i);
        }
        FLIP_SLOT *const slot = Room_GetFlipSlot(i);
        slot->mask = record->slot_masks[i] & TRIGGER_MASK_ALL;
        slot->is_one_shot = (record->one_shot_slots & (1 << i)) != 0;
    }
    Room_SetFlipStatus((record->groups & M_FLIP_STATUS_BIT) != 0);
    m_World.sent_flip = M_EncodeFlip();
}
