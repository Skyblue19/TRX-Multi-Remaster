#include <trx/game/coop/creatures.h>

#include <trx/core/memory.h>
#include <trx/core/utils.h>
#include <trx/game/anims.h>
#include <trx/game/collision/common.h>
#include <trx/game/coop/puppet.h>
#include <trx/game/creature.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/objects.h>
#include <trx/game/pathing/lot.h>
#include <trx/game/rooms.h>

#include <string.h>

// A creature keeps after the player it goes for until another one is half
// as near again: one that turned at every step between two players standing
// alike would never reach either.
#define M_SWITCH_NUMERATOR 4
#define M_SWITCH_DENOMINATOR 9
// How near a puppet a creature has to be for its spheres to be worth testing
// against the puppet's.
#define M_TOUCH_RANGE (WALL_L * 3)
#define M_NO_FRAME INT32_MIN
#define M_NO_TARGET (-1)

typedef enum {
    M_FLAG_SIMULATED = 1 << 0,
    M_FLAG_VISIBLE = 1 << 1,
    M_FLAG_FINISHED = 1 << 2,
} M_FLAG;

static struct {
    // Whether the level has been learnt since it loaded.
    bool known;
    int32_t item_count;
    // On the host, what the others were last told of each creature, and the
    // puppet slot each one goes after, or M_NO_TARGET for Lara.
    COOP_CREATURE_RECORD *sent;
    int8_t *targets;
    // On a client, the host's last word on each creature, still to apply,
    // and the host frame it was stamped with.
    COOP_CREATURE_RECORD *received;
    bool *pending;
    int32_t *received_frame;
    int32_t stand_in_slot;
    // On the host, whether Lara is at the exit, out of the creatures' reach.
    bool lara_away;
} m_Creatures = {
    .stand_in_slot = M_NO_TARGET,
};

// The host's frame count, which goes on across levels so that a stamp from
// an earlier level never passes for a later one.
static int32_t m_Frame = 0;

static COOP_CREATURE_RECORD M_Encode(const int16_t item_num)
{
    const ITEM *const item = Item_Get(item_num);
    uint8_t flags = 0;
    flags |= item->is_simulated ? M_FLAG_SIMULATED : 0;
    flags |= item->is_visible ? M_FLAG_VISIBLE : 0;
    flags |= item->is_finished ? M_FLAG_FINISHED : 0;
    COOP_CREATURE_RECORD record = {
        .item_num = item_num,
        .object_id = item->object_id,
        .flags = flags,
        .current_anim_state = item->current_anim_state,
        .goal_anim_state = item->goal_anim_state,
        .anim_num = item->anim_num,
        .frame_num = item->frame_num,
        .hit_points = item->hit_points,
        .room_num = item->room_num,
        .pos = item->pos,
        .rot = item->rot,
        .mesh_bits = item->mesh_bits,
    };
    const CREATURE *const creature = item->creature_data;
    if (creature != nullptr) {
        for (int32_t i = 0; i < 4; i++) {
            record.joints[i] = creature->joint_rotation[i];
        }
    }
    return record;
}

// Only the discrete part counts as a change. A creature on the move turns
// and steps every frame, and that goes in the stream of moving ones.
static bool M_IsSameState(
    const COOP_CREATURE_RECORD *const a, const COOP_CREATURE_RECORD *const b)
{
    return a->object_id == b->object_id && a->flags == b->flags
        && a->current_anim_state == b->current_anim_state
        && a->goal_anim_state == b->goal_anim_state
        && a->hit_points == b->hit_points && a->mesh_bits == b->mesh_bits;
}

static void M_Learn(void)
{
    const int32_t count = Item_GetLevelCount();
    const size_t slots = MAX(count, 1);
    m_Creatures.item_count = count;
    m_Creatures.sent = Memory_Alloc(sizeof(COOP_CREATURE_RECORD) * slots);
    m_Creatures.targets = Memory_Alloc(sizeof(int8_t) * slots);
    m_Creatures.received = Memory_Alloc(sizeof(COOP_CREATURE_RECORD) * slots);
    m_Creatures.pending = Memory_Alloc(sizeof(bool) * slots);
    m_Creatures.received_frame = Memory_Alloc(sizeof(int32_t) * slots);
    for (int32_t i = 0; i < count; i++) {
        if (Coop_Creatures_IsShared(i)) {
            m_Creatures.sent[i] = M_Encode(i);
        }
        m_Creatures.targets[i] = M_NO_TARGET;
        m_Creatures.pending[i] = false;
        m_Creatures.received_frame[i] = M_NO_FRAME;
    }
    m_Creatures.known = true;
}

static void M_EnsureKnown(void)
{
    if (!m_Creatures.known) {
        M_Learn();
    }
}

void Coop_Creatures_Reset(void)
{
    Memory_FreePointer(&m_Creatures.sent);
    Memory_FreePointer(&m_Creatures.targets);
    Memory_FreePointer(&m_Creatures.received);
    Memory_FreePointer(&m_Creatures.pending);
    Memory_FreePointer(&m_Creatures.received_frame);
    m_Creatures = (typeof(m_Creatures)) {
        .stand_in_slot = M_NO_TARGET,
    };
}

bool Coop_Creatures_IsShared(const int16_t item_num)
{
    if (item_num < 0 || item_num >= Item_GetLevelCount()) {
        return false;
    }
    if (item_num == Lara_GetLaraInfo()->item_num
        || Coop_Puppet_IsItem(item_num)) {
        return false;
    }
    const ITEM *const item = Item_Get(item_num);
    return item->object_id != O_LARA
        && Object_Get(item->object_id)->intelligent;
}

int32_t Coop_Creatures_GetFrame(void)
{
    return m_Frame;
}

void Coop_Creatures_Take(VECTOR *const moving, VECTOR *const changed)
{
    Vector_Clear(moving);
    Vector_Clear(changed);
    m_Frame++;
    if (!m_Creatures.known) {
        M_Learn();
        return;
    }

    for (int32_t i = 0; i < m_Creatures.item_count; i++) {
        if (!Coop_Creatures_IsShared(i)) {
            continue;
        }
        const COOP_CREATURE_RECORD record = M_Encode(i);
        if (!M_IsSameState(&record, &m_Creatures.sent[i])) {
            m_Creatures.sent[i] = record;
            Vector_Add(changed, &record);
        } else if (Item_Get(i)->is_simulated) {
            Vector_Add(moving, &record);
        }
    }
}

void Coop_Creatures_GetAll(VECTOR *const records)
{
    Vector_Clear(records);
    const int32_t count = Item_GetLevelCount();
    for (int32_t i = 0; i < count; i++) {
        if (Coop_Creatures_IsShared(i)) {
            const COOP_CREATURE_RECORD record = M_Encode(i);
            Vector_Add(records, &record);
        }
    }
}

void Coop_Creatures_Receive(
    const COOP_CREATURE_RECORD *const record, const int32_t frame)
{
    // Learnt in play only: a packet can land while the level is still to
    // load, and the host sends the moving creatures again next frame.
    if (!m_Creatures.known) {
        return;
    }
    const int16_t item_num = record->item_num;
    if (item_num < 0 || item_num >= m_Creatures.item_count
        || frame < m_Creatures.received_frame[item_num]) {
        return;
    }
    m_Creatures.received_frame[item_num] = frame;
    m_Creatures.received[item_num] = *record;
    m_Creatures.pending[item_num] = true;
}

static void M_ApplySimulated(const int16_t item_num, const bool simulated)
{
    ITEM *const item = Item_Get(item_num);
    if (simulated && !item->is_simulated) {
        Item_AddSimulated(item_num);
        // The gun code picks its targets among the creatures with AI, so a
        // creature gets one here too, though it never runs.
        if (item->creature_data == nullptr) {
            LOT_EnableBaddieAI(item_num, true);
        }
    } else if (!simulated && item->is_simulated) {
        if (item->creature_data != nullptr) {
            LOT_DisableBaddieAI(item_num);
        }
        Item_RemoveSimulated(item_num);
    }
}

static void M_ApplyAnim(
    ITEM *const item, const COOP_CREATURE_RECORD *const record)
{
    item->current_anim_state = record->current_anim_state;
    item->goal_anim_state = record->goal_anim_state;
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

static void M_Apply(const COOP_CREATURE_RECORD *const record)
{
    const int16_t item_num = record->item_num;
    if (!Coop_Creatures_IsShared(item_num)) {
        return;
    }
    ITEM *const item = Item_Get(item_num);
    if (item->is_destroyed) {
        return;
    }

    // A creature can turn into another, as one that leaves the water.
    if (record->object_id != item->object_id) {
        if (record->object_id < 0 || record->object_id >= Object_GetCount()) {
            return;
        }
        const OBJECT *const obj = Object_Get(record->object_id);
        if (!obj->loaded || !obj->intelligent) {
            return;
        }
        item->object_id = record->object_id;
    }

    const uint8_t flags = record->flags;
    Item_SetVisible(item, (flags & M_FLAG_VISIBLE) != 0);
    item->is_finished = (flags & M_FLAG_FINISHED) != 0;
    M_ApplySimulated(item_num, (flags & M_FLAG_SIMULATED) != 0);
    M_ApplyAnim(item, record);
    item->hit_points = record->hit_points;
    item->mesh_bits = record->mesh_bits;

    if (record->room_num < 0 || record->room_num >= Room_GetCount()) {
        return;
    }
    if (item->is_present && item->room_num != record->room_num) {
        Item_UpdateRoom(item_num, record->room_num);
    }
    item->pos = record->pos;
    item->rot = record->rot;
    // The shadow is laid on the floor under the creature, which its control
    // works out where it runs.
    int16_t floor_room_num = item->room_num;
    const SECTOR *const sector = Room_GetSector(item->pos, &floor_room_num);
    item->floor = Room_GetHeight(sector, item->pos);

    CREATURE *const creature = item->creature_data;
    if (creature != nullptr) {
        for (int32_t i = 0; i < 4; i++) {
            creature->joint_rotation[i] = record->joints[i];
        }
    }
}

void Coop_Creatures_ApplyReceived(void)
{
    M_EnsureKnown();
    for (int32_t i = 0; i < m_Creatures.item_count; i++) {
        if (m_Creatures.pending[i]) {
            m_Creatures.pending[i] = false;
            M_Apply(&m_Creatures.received[i]);
        }
    }
}

static int64_t M_GetDistanceSq(const ITEM *const a, const ITEM *const b)
{
    const int64_t dx = a->pos.x - b->pos.x;
    const int64_t dy = a->pos.y - b->pos.y;
    const int64_t dz = a->pos.z - b->pos.z;
    return dx * dx + dy * dy + dz * dz;
}

static int32_t M_ChooseTarget(const int16_t item_num)
{
    const ITEM *const item = Item_Get(item_num);
    const ITEM *const lara_item = Lara_GetItem();
    const int32_t current = m_Creatures.targets[item_num];

    int32_t best = M_NO_TARGET;
    int64_t best_dist = lara_item->hit_points > 0 && !m_Creatures.lara_away
        ? M_GetDistanceSq(item, lara_item)
        : INT64_MAX;
    int64_t current_dist = current == M_NO_TARGET ? best_dist : INT64_MAX;
    for (int32_t slot = 0; slot < COOP_MAX_PUPPETS; slot++) {
        const int16_t puppet_num = Coop_Puppet_GetItemNum(slot);
        if (puppet_num == NO_ITEM) {
            continue;
        }
        const ITEM *const puppet = Item_Get(puppet_num);
        if (puppet->hit_points <= 0) {
            continue;
        }
        const int64_t dist = M_GetDistanceSq(item, puppet);
        if (slot == current) {
            current_dist = dist;
        }
        if (dist < best_dist) {
            best = slot;
            best_dist = dist;
        }
    }

    if (best != current && current_dist != INT64_MAX
        && current_dist * M_SWITCH_NUMERATOR
            <= best_dist * M_SWITCH_DENOMINATOR) {
        return current;
    }
    return best;
}

void Coop_Creatures_BeginControl(const int16_t item_num)
{
    m_Creatures.stand_in_slot = M_NO_TARGET;
    if (!Coop_Creatures_IsShared(item_num)) {
        return;
    }
    M_EnsureKnown();
    if (item_num >= m_Creatures.item_count) {
        return;
    }

    ITEM *const item = Item_Get(item_num);
    // Allies pick their own targets among the other creatures.
    if (item->creature_data == nullptr || Creature_IsAlly(item)
        || Creature_IsAllyTargetingEnemy(item)) {
        return;
    }

    const int32_t target = M_ChooseTarget(item_num);
    m_Creatures.targets[item_num] = (int8_t)target;
    if (target == M_NO_TARGET) {
        return;
    }

    ITEM *const puppet = Item_Get(Coop_Puppet_GetItemNum(target));
    m_Creatures.stand_in_slot = target;
    Lara_SetStandIn(puppet);
    // What the creature touches was worked out against Lara, as she moved;
    // against the puppet it is worked out here.
    item->touch_bits = 0;
    if (M_GetDistanceSq(item, puppet) < SQUARE((int64_t)M_TOUCH_RANGE)) {
        Collide_TestCollision(item, puppet);
    }
}

void Coop_Creatures_EndControl(void)
{
    if (m_Creatures.stand_in_slot != M_NO_TARGET) {
        Lara_SetStandIn(nullptr);
        m_Creatures.stand_in_slot = M_NO_TARGET;
    }
}

int32_t Coop_Creatures_GetStandInSlot(void)
{
    return m_Creatures.stand_in_slot;
}

void Coop_Creatures_SetLaraAway(const bool away)
{
    m_Creatures.lara_away = away;
}

bool Coop_Creatures_IsLaraAway(void)
{
    return m_Creatures.lara_away;
}
