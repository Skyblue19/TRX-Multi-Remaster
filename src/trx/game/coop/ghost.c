#include <trx/game/coop/ghost.h>

#include <trx/core/log.h>
#include <trx/core/result.h>
#include <trx/core/utils.h>
#include <trx/game/game.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/objects.h>
#include <trx/game/shell.h>

// One logic frame of a player, as much as a puppet needs to stand where the
// player stood and strike the same pose. It is what a remote player will send.
typedef struct {
    XYZ_32 pos;
    XYZ_16 rot;
    int16_t room_num;
    // Relative to Lara's own animations, so it holds for any object that
    // borrows them.
    int16_t anim_num;
    int16_t frame_num;
} M_PLAYER_STATE;

// A power of two, so the ring index wraps with a mask. At 30 frames a second
// it holds a little over eight seconds of Lara.
#define M_HISTORY_SIZE 256
#define M_DEFAULT_DELAY 30

static struct {
    OBJECT_ID object_id;
    int16_t item_num;
    M_PLAYER_STATE history[M_HISTORY_SIZE];
    int32_t recorded;
} m_Ghost = {
    .object_id = NO_OBJECT,
    .item_num = NO_ITEM,
};

static int32_t M_GetDelay(void)
{
    int32_t delay = Shell_GetArgs()->coop.ghost_delay;
    if (delay <= 0) {
        return M_DEFAULT_DELAY;
    }
    CLAMP(delay, 1, M_HISTORY_SIZE - 1);
    return delay;
}

static M_PLAYER_STATE M_CaptureState(const ITEM *const lara_item)
{
    return (M_PLAYER_STATE) {
        .pos = lara_item->pos,
        .rot = lara_item->rot,
        .room_num = lara_item->room_num,
        .anim_num = Item_GetRelativeAnim(lara_item),
        .frame_num = Item_GetRelativeFrame(lara_item),
    };
}

static void M_ApplyState(ITEM *const item, const M_PLAYER_STATE *const state)
{
    Item_SwitchToObjAnim(item, state->anim_num, state->frame_num, O_LARA);
    item->pos = state->pos;
    item->rot = state->rot;
    if (item->room_num != state->room_num) {
        Item_UpdateRoom(Item_GetIndex(item), state->room_num);
    }
}

static bool M_Spawn(const ITEM *const lara_item)
{
    // The minted object is the level's, like the gold skin's twins, so it is
    // made again for each level rather than once.
    if (m_Ghost.object_id == NO_OBJECT) {
        const OBJECT_ID object_id = Object_Mint();
        if (!SHOULD(
                Object_BorrowContent(object_id, O_LARA),
                "Coop ghost: cannot borrow Lara")) {
            return false;
        }
        OBJECT *const obj = Object_Get(object_id);
        obj->draw_func = Object_DrawAnimatingItem;
        obj->shadow_size = Object_Get(O_LARA)->shadow_size;
        m_Ghost.object_id = object_id;
    }

    const int16_t item_num = Item_Create();
    if (item_num == NO_ITEM) {
        LOG_WARNING("Coop ghost: no free item slot");
        return false;
    }

    ITEM *const item = Item_Get(item_num);
    item->object_id = m_Ghost.object_id;
    item->room_num = lara_item->room_num;
    item->pos = lara_item->pos;
    item->rot = lara_item->rot;
    Item_Initialise(item_num);
    // A puppet stands wherever its player does, through walls included, so
    // nothing should bump into it. Nothing simulates it either: this module
    // moves it.
    item->is_collidable = false;

    m_Ghost.item_num = item_num;
    LOG_INFO("Coop ghost spawned as item %d", item_num);
    return true;
}

void Coop_Ghost_Control(void)
{
    if (!Shell_GetArgs()->coop.ghost || !Game_IsPlaying()) {
        return;
    }

    const ITEM *const lara_item = Lara_GetItem();
    if (lara_item == nullptr) {
        return;
    }

    m_Ghost.history[m_Ghost.recorded & (M_HISTORY_SIZE - 1)] =
        M_CaptureState(lara_item);
    m_Ghost.recorded++;

    const int32_t delay = M_GetDelay();
    if (m_Ghost.recorded <= delay) {
        return;
    }

    if (m_Ghost.item_num == NO_ITEM && !M_Spawn(lara_item)) {
        return;
    }

    const int32_t replayed = m_Ghost.recorded - 1 - delay;
    M_ApplyState(
        Item_Get(m_Ghost.item_num),
        &m_Ghost.history[replayed & (M_HISTORY_SIZE - 1)]);
}

void Coop_Ghost_Reset(void)
{
    m_Ghost.object_id = NO_OBJECT;
    m_Ghost.item_num = NO_ITEM;
    m_Ghost.recorded = 0;
}

bool Coop_Ghost_IsItem(const int16_t item_num)
{
    return item_num != NO_ITEM && item_num == m_Ghost.item_num;
}
