#include <trx/game/coop/puppet.h>

#include <trx/core/log.h>
#include <trx/core/result.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/objects.h>
#include <trx/game/rooms.h>

static_assert(COOP_MAX_PUPPETS == 4, "spell out every puppet slot below");

static struct {
    // Minted once per level for all the puppets, like the gold skin's twins.
    OBJECT_ID object_id;
    int16_t item_nums[COOP_MAX_PUPPETS];
} m_Puppets = {
    .object_id = NO_OBJECT,
    .item_nums = { NO_ITEM, NO_ITEM, NO_ITEM, NO_ITEM },
};

static bool M_IsValidSlot(const int32_t slot)
{
    return slot >= 0 && slot < COOP_MAX_PUPPETS;
}

// A state from the network is only as good as the sender, so everything that
// indexes the level's tables is checked before it is used.
static bool M_IsValidState(const COOP_PLAYER_STATE *const state)
{
    const OBJECT *const lara_obj = Object_Get(O_LARA);
    return state->room_num >= 0 && state->room_num < Room_GetCount()
        && state->anim_num >= 0 && state->anim_num < lara_obj->anim_count
        && state->frame_num >= 0;
}

static bool M_EnsureObject(void)
{
    if (m_Puppets.object_id != NO_OBJECT) {
        return true;
    }
    const OBJECT_ID object_id = Object_Mint();
    if (!SHOULD(
            Object_BorrowContent(object_id, O_LARA),
            "Coop: cannot borrow Lara for the puppets")) {
        return false;
    }
    OBJECT *const obj = Object_Get(object_id);
    // Minted objects get none of the defaults the catalogued ones are set up
    // with, so the draw is named here.
    obj->draw_func = Object_DrawAnimatingItem;
    obj->shadow_size = Object_Get(O_LARA)->shadow_size;
    m_Puppets.object_id = object_id;
    return true;
}

static int16_t M_Spawn(const COOP_PLAYER_STATE *const state)
{
    if (!M_EnsureObject()) {
        return NO_ITEM;
    }

    const int16_t item_num = Item_Create();
    if (item_num == NO_ITEM) {
        LOG_WARNING("Coop: no free item slot for a puppet");
        return NO_ITEM;
    }

    ITEM *const item = Item_Get(item_num);
    item->object_id = m_Puppets.object_id;
    item->room_num = state->room_num;
    item->pos = state->pos;
    item->rot = state->rot;
    Item_Initialise(item_num);
    // A puppet stands wherever its player does, through walls included, so
    // nothing should bump into it. Nothing simulates it either: its player's
    // state moves it.
    item->is_collidable = false;
    LOG_INFO("Coop: puppet spawned as item %d", item_num);
    return item_num;
}

COOP_PLAYER_STATE Coop_Puppet_CaptureLara(void)
{
    const ITEM *const lara_item = Lara_GetItem();
    return (COOP_PLAYER_STATE) {
        .pos = lara_item->pos,
        .rot = lara_item->rot,
        .room_num = lara_item->room_num,
        .anim_num = Item_GetRelativeAnim(lara_item),
        .frame_num = Item_GetRelativeFrame(lara_item),
    };
}

bool Coop_Puppet_Apply(
    const int32_t slot, const COOP_PLAYER_STATE *const state)
{
    if (!M_IsValidSlot(slot) || !Object_Get(O_LARA)->loaded
        || !M_IsValidState(state)) {
        return false;
    }

    if (m_Puppets.item_nums[slot] == NO_ITEM) {
        m_Puppets.item_nums[slot] = M_Spawn(state);
        if (m_Puppets.item_nums[slot] == NO_ITEM) {
            return false;
        }
    }

    const int16_t item_num = m_Puppets.item_nums[slot];
    ITEM *const item = Item_Get(item_num);
    Item_SwitchToObjAnim(item, state->anim_num, state->frame_num, O_LARA);
    const ANIM *const anim = Item_GetAnim(item);
    if (item->frame_num > anim->frame_end) {
        item->frame_num = anim->frame_end;
    }
    item->pos = state->pos;
    item->rot = state->rot;
    if (item->room_num != state->room_num) {
        Item_UpdateRoom(item_num, state->room_num);
    }
    return true;
}

void Coop_Puppet_Remove(const int32_t slot)
{
    if (!M_IsValidSlot(slot) || m_Puppets.item_nums[slot] == NO_ITEM) {
        return;
    }
    Item_Destroy(m_Puppets.item_nums[slot]);
    m_Puppets.item_nums[slot] = NO_ITEM;
}

void Coop_Puppet_Reset(void)
{
    m_Puppets.object_id = NO_OBJECT;
    for (int32_t i = 0; i < COOP_MAX_PUPPETS; i++) {
        m_Puppets.item_nums[i] = NO_ITEM;
    }
}

bool Coop_Puppet_IsItem(const int16_t item_num)
{
    if (item_num == NO_ITEM) {
        return false;
    }
    for (int32_t i = 0; i < COOP_MAX_PUPPETS; i++) {
        if (m_Puppets.item_nums[i] == item_num) {
            return true;
        }
    }
    return false;
}
