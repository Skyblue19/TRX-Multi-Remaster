#include <trx/game/coop/puppet.h>

#include <trx/config.h>
#include <trx/core/log.h>
#include <trx/core/math/const.h>
#include <trx/core/result.h>
#include <trx/core/utils.h>
#include <trx/game/anims.h>
#include <trx/game/coop/identity.h>
#include <trx/game/gun/registry.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/matrix.h>
#include <trx/game/objects.h>
#include <trx/game/output/vars.h>
#include <trx/game/rooms.h>
#include <trx/game/ui/common.h>
#include <trx/game/ui/draw.h>
#include <trx/game/ui/scaler.h>
#include <trx/game/ui/text.h>
#include <trx/game/viewport.h>

#include <math.h>
#include <string.h>

static_assert(COOP_MAX_PUPPETS == 4, "spell out every puppet slot below");

// How far above a puppet's feet its label sits, a little over Lara's height.
#define M_LABEL_HEIGHT 900
#define M_LABEL_TEXT_SCALE 0.8f
// The health bar under the name, in UI canvas units.
#define M_BAR_WIDTH 40.0f
#define M_BAR_HEIGHT 3.0f
#define M_BAR_GAP 2.0f

static struct {
    // Minted once per level for all the puppets, like the gold skin's twins.
    OBJECT_ID object_id;
    int16_t item_nums[COOP_MAX_PUPPETS];
    // How each puppet looks, resolved against this level, and whether it has
    // been told yet.
    LARA_APPEARANCE appearances[COOP_MAX_PUPPETS];
    bool has_appearance[COOP_MAX_PUPPETS];
    // What the label over each puppet shows.
    char names[COOP_MAX_PUPPETS][COOP_NAME_SIZE];
    int16_t hit_points[COOP_MAX_PUPPETS];
} m_Puppets = {
    .object_id = NO_OBJECT,
    .item_nums = { NO_ITEM, NO_ITEM, NO_ITEM, NO_ITEM },
};

static bool M_IsValidSlot(const int32_t slot)
{
    return slot >= 0 && slot < COOP_MAX_PUPPETS;
}

static int32_t M_FindSlot(const ITEM *const item)
{
    const int16_t item_num = Item_GetIndex(item);
    for (int32_t i = 0; i < COOP_MAX_PUPPETS; i++) {
        if (m_Puppets.item_nums[i] == item_num) {
            return i;
        }
    }
    return -1;
}

static bool M_Draw(const ITEM *const item)
{
    const int32_t slot = M_FindSlot(item);
    if (slot < 0 || !m_Puppets.has_appearance[slot]) {
        return Object_DrawAnimatingItem(item);
    }
    return Lara_DrawAppearance(item, &m_Puppets.appearances[slot]);
}

// A state from the network is only as good as the sender, so everything that
// indexes the level's tables is checked before it is used.
static bool M_IsValidBody(const COOP_PLAYER_STATE *const state)
{
    const OBJECT *const lara_obj = Object_Get(O_LARA);
    return state->room_num >= 0 && state->room_num < Room_GetCount()
        && state->anim_num >= 0 && state->anim_num < lara_obj->anim_count
        && state->frame_num >= 0;
}

static COOP_ARM_STATE M_EncodeArm(const LARA_ARM *const arm)
{
    return (COOP_ARM_STATE) {
        .frame_base = Anim_GetFrameIndex(arm->frame_base),
        .frame_num = arm->frame_num,
        .anim_num = arm->anim_num,
        .rot = arm->rot,
        .flash_gun = arm->flash_gun,
    };
}

// Whether the draw can read the arm's frame. The pistols and the flare count
// frames from the start of the arm's animation, the rifles from frame_base.
static bool M_DecodeArm(
    const COOP_ARM_STATE *const in, const bool counts_from_anim,
    LARA_ARM *const out)
{
    ANIM_FRAME *const frame_base = Anim_GetFrameByIndex(in->frame_base);
    if (frame_base == nullptr || in->anim_num < 0
        || in->anim_num >= Anim_GetTotalCount()) {
        return false;
    }

    int32_t frame_idx = in->frame_base + in->frame_num;
    if (counts_from_anim) {
        frame_idx -= Anim_GetAnim(in->anim_num)->frame_base;
    }
    if (Anim_GetFrameByIndex(frame_idx) == nullptr) {
        return false;
    }

    *out = (LARA_ARM) {
        .frame_base = frame_base,
        .frame_num = in->frame_num,
        .anim_num = in->anim_num,
        .rot = in->rot,
        .flash_gun = in->flash_gun,
    };
    return true;
}

static OBJECT_MESH *M_DecodeMesh(const int32_t mesh_idx)
{
    if (mesh_idx < 0 || mesh_idx >= Object_GetMeshCount()) {
        return nullptr;
    }
    return Object_GetMesh(mesh_idx);
}

static bool M_IsDrawingGun(const LARA_GUN_STATE gun_status)
{
    return gun_status == LGS_READY || gun_status == LGS_SPECIAL
        || gun_status == LGS_DRAW || gun_status == LGS_UNDRAW;
}

// The arms are only read for a gun in hand or a lit flare, so only then must
// they make sense. When they do not, the puppet drops to empty hands.
static void M_DecodeArms(
    const COOP_PLAYER_STATE *const state, LARA_APPEARANCE *const out)
{
    out->gun_status = LGS_ARMLESS;
    out->gun_type = LGT_UNARMED;
    out->flare_control = false;

    const LARA_GUN_TYPE gun_type = state->gun_type;
    if (gun_type == LGT_UNARMED || !Gun_Registry_IsValidType(gun_type)) {
        return;
    }
    const WEAPON_INFO *const weapon = Gun_Registry_Get(gun_type);

    if (M_IsDrawingGun(state->gun_status)) {
        const bool counts_from_anim = weapon->type != WEAPON_TYPE_RIFLE;
        if (!M_DecodeArm(&state->left_arm, counts_from_anim, &out->left_arm)
            || !M_DecodeArm(
                &state->right_arm, counts_from_anim, &out->right_arm)) {
            return;
        }
        out->gun_status = state->gun_status;
        out->gun_type = gun_type;
        out->flare_control = state->flare_control;
        return;
    }

    if (state->flare_control
        && M_DecodeArm(&state->left_arm, true, &out->left_arm)) {
        out->gun_type = gun_type;
        out->flare_control = true;
    }
}

static void M_DecodeAppearance(
    const COOP_PLAYER_STATE *const state, LARA_APPEARANCE *const out)
{
    *out = (LARA_APPEARANCE) {
        .head_rot = state->head_rot,
        .torso_rot = state->torso_rot,
    };
    M_DecodeArms(state, out);

    // The body's own meshes stand in for any the sender wore that this copy
    // of the level does not have.
    const OBJECT *const puppet_obj = Object_Get(m_Puppets.object_id);
    for (LARA_MESH mesh = LM_FIRST; mesh < LM_NUMBER_OF; mesh++) {
        OBJECT_MESH *const worn = M_DecodeMesh(state->meshes[mesh]);
        out->meshes[mesh] = worn != nullptr
            ? worn
            : Object_GetMesh(puppet_obj->mesh_idx + mesh);

        const COOP_EQUIPMENT_STATE *const equipment = &state->equipment[mesh];
        const OBJECT_MESH *const equipment_mesh =
            M_DecodeMesh(equipment->mesh);
        out->equipment[mesh] = (LARA_SKIN_EQUIPMENT) {
            .visible = equipment->visible && equipment_mesh != nullptr,
            .mesh = equipment_mesh,
            .offset = equipment->offset,
        };
    }
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
    obj->draw_func = M_Draw;
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
    const LARA_APPEARANCE appearance = Lara_GetAppearance();
    COOP_PLAYER_STATE state = {
        .pos = lara_item->pos,
        .rot = lara_item->rot,
        .room_num = lara_item->room_num,
        .anim_num = Item_GetRelativeAnim(lara_item),
        .frame_num = Item_GetRelativeFrame(lara_item),
        .hit_points = lara_item->hit_points,
        .gun_status = appearance.gun_status,
        .gun_type = appearance.gun_type,
        .head_rot = appearance.head_rot,
        .torso_rot = appearance.torso_rot,
        .left_arm = M_EncodeArm(&appearance.left_arm),
        .right_arm = M_EncodeArm(&appearance.right_arm),
        .flare_control = appearance.flare_control,
    };
    for (LARA_MESH mesh = LM_FIRST; mesh < LM_NUMBER_OF; mesh++) {
        state.meshes[mesh] = appearance.meshes[mesh] != nullptr
            ? Object_GetMeshIndex(appearance.meshes[mesh])
            : -1;
        const LARA_SKIN_EQUIPMENT *const equipment =
            &appearance.equipment[mesh];
        state.equipment[mesh] = (COOP_EQUIPMENT_STATE) {
            .visible = equipment->visible && equipment->mesh != nullptr,
            .mesh = equipment->mesh != nullptr
                ? Object_GetMeshIndex(equipment->mesh)
                : -1,
            .offset = equipment->offset,
        };
    }
    return state;
}

bool Coop_Puppet_Apply(
    const int32_t slot, const COOP_PLAYER_STATE *const state)
{
    if (!M_IsValidSlot(slot) || !Object_Get(O_LARA)->loaded
        || !M_IsValidBody(state)) {
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
    // The shadow is laid on the floor the item stands over, which nothing
    // else works out for a puppet.
    int16_t floor_room_num = state->room_num;
    const SECTOR *const sector = Room_GetSector(state->pos, &floor_room_num);
    item->floor = Room_GetHeight(sector, state->pos);

    M_DecodeAppearance(state, &m_Puppets.appearances[slot]);
    m_Puppets.has_appearance[slot] = true;
    m_Puppets.hit_points[slot] = state->hit_points;
    return true;
}

void Coop_Puppet_SetName(const int32_t slot, const char *const name)
{
    if (!M_IsValidSlot(slot)) {
        return;
    }
    strncpy(m_Puppets.names[slot], name, COOP_NAME_SIZE - 1);
    m_Puppets.names[slot][COOP_NAME_SIZE - 1] = '\0';
}

// Where a point in the world falls on the UI canvas, or false behind the
// camera. The scene is rasterized on its own pixel grid, so the point is
// placed by where it falls within that grid and carried over to the UI's.
static bool M_ProjectToCanvas(
    const XYZ_32 pos, float *const out_x, float *const out_y)
{
    const MATRIX *const m = &g_ViewMatrix;
    const double w2v = (double)(1 << W2V_SHIFT);
    // Relative to the camera, as absolute coordinates overflow the products.
    const XYZ_32 rel = {
        .x = pos.x - g_ViewPos.x,
        .y = pos.y - g_ViewPos.y,
        .z = pos.z - g_ViewPos.z,
    };
    const double zv = ((double)m->_20 * rel.x + (double)m->_21 * rel.y
                       + (double)m->_22 * rel.z)
        / w2v;
    if (zv <= 0.0) {
        return false;
    }
    const double xv = ((double)m->_00 * rel.x + (double)m->_01 * rel.y
                       + (double)m->_02 * rel.z)
        / w2v;
    const double yv = ((double)m->_10 * rel.x + (double)m->_11 * rel.y
                       + (double)m->_12 * rel.z)
        / w2v;

    const double game_x =
        Viewport_GetCenterX(VIEWPORT_GAME) + xv * g_PhdPersp / zv;
    const double game_y =
        Viewport_GetCenterY(VIEWPORT_GAME) + yv * g_PhdPersp / zv;
    const double rel_x = (game_x - Viewport_GetMinX(VIEWPORT_GAME))
        / Viewport_GetWidth(VIEWPORT_GAME);
    const double rel_y = (game_y - Viewport_GetMinY(VIEWPORT_GAME))
        / Viewport_GetHeight(VIEWPORT_GAME);
    if (rel_x < 0.0 || rel_x > 1.0 || rel_y < 0.0 || rel_y > 1.0) {
        return false;
    }

    const float ui_x = (float)(rel_x * Viewport_GetWidth(VIEWPORT_UI));
    const float ui_y = (float)(rel_y * Viewport_GetHeight(VIEWPORT_UI));
    *out_x = UI_Scaler_CalcInverse(ui_x * 0x10000, UI_SCALER_TARGET_GENERIC)
        / 0x10000;
    *out_y = UI_Scaler_CalcInverse(ui_y * 0x10000, UI_SCALER_TARGET_GENERIC)
        / 0x10000;
    return true;
}

// A bar from green at full health to red near none, over a dark backing.
static void M_DrawHealthBar(const float x, const float y, const int16_t hp)
{
    int32_t shown_hp = hp;
    CLAMP(shown_hp, 0, LARA_MAX_HITPOINTS);
    const float ratio = shown_hp / (float)LARA_MAX_HITPOINTS;
    const int32_t sx = lroundf(UI_ScaleX(x - M_BAR_WIDTH / 2));
    const int32_t sy = lroundf(UI_ScaleY(y));
    const int32_t sw = lroundf(UI_ScaleX(M_BAR_WIDTH));
    const int32_t sh = MAX(1, lroundf(UI_ScaleY(M_BAR_HEIGHT)));
    UI_ScheduleDrawScreenFlatQuad(
        sx - 1, sy - 1, 0, sw + 2, sh + 2,
        (RGBA_8888) { .r = 0, .g = 0, .b = 0, .a = 0xA0 });
    const RGBA_8888 fill = {
        .r = (uint8_t)(0xFF * (1.0f - ratio)),
        .g = (uint8_t)(0xD0 * ratio),
        .b = 0x20,
        .a = 0xFF,
    };
    const int32_t fill_w = lroundf(sw * ratio);
    if (fill_w > 0) {
        UI_ScheduleDrawScreenFlatQuad(sx, sy, 0, fill_w, sh, fill);
    }
}

void Coop_Puppet_DrawLabels(void)
{
    if (!g_Config.ui.enable_coop_nameplates) {
        return;
    }
    const UI_TEXT_SETTINGS settings = {
        .scale = M_LABEL_TEXT_SCALE,
        .z = 0,
    };
    // The interface draws only into a layer; the labels sit under the HUD.
    UI_BeginLayer(UI_LAYER_UNDER);
    for (int32_t i = 0; i < COOP_MAX_PUPPETS; i++) {
        if (m_Puppets.item_nums[i] == NO_ITEM) {
            continue;
        }
        const ITEM *const item = Item_Get(m_Puppets.item_nums[i]);
        XYZ_32 head = item->interp.result.pos;
        head.y -= M_LABEL_HEIGHT;
        float x;
        float y;
        if (!M_ProjectToCanvas(head, &x, &y)) {
            continue;
        }

        const char *const name = m_Puppets.names[i];
        if (name[0] != '\0') {
            float text_w;
            float text_h;
            UI_Text_Measure(name, &text_w, &text_h, settings);
            UI_Text_Draw(name, x - text_w / 2, y - text_h, settings);
        }
        M_DrawHealthBar(x, y + M_BAR_GAP, m_Puppets.hit_points[i]);
    }
    UI_EndLayer(UI_LAYER_UNDER);
}

void Coop_Puppet_Remove(const int32_t slot)
{
    if (!M_IsValidSlot(slot) || m_Puppets.item_nums[slot] == NO_ITEM) {
        return;
    }
    Item_Destroy(m_Puppets.item_nums[slot]);
    m_Puppets.item_nums[slot] = NO_ITEM;
    m_Puppets.has_appearance[slot] = false;
}

// Names outlive the level; the other players do not change with it.

void Coop_Puppet_Reset(void)
{
    m_Puppets.object_id = NO_OBJECT;
    for (int32_t i = 0; i < COOP_MAX_PUPPETS; i++) {
        m_Puppets.item_nums[i] = NO_ITEM;
        m_Puppets.has_appearance[i] = false;
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
