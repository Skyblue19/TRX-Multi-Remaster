#include <trx/game/coop/profile.h>

#include <trx/core/bson.h>
#include <trx/core/json/util/read_io.h>
#include <trx/core/json/util/write_io.h>
#include <trx/core/log.h>
#include <trx/core/memory.h>
#include <trx/core/result.h>
#include <trx/core/utils.h>
#include <trx/game/game.h>
#include <trx/game/game_flow.h>
#include <trx/game/gun/registry.h>
#include <trx/game/inventory.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/lara/cheat.h>
#include <trx/game/rooms.h>
#include <trx/game/savegame.h>
#include <trx/game/savegame/file.h>

#include <zconf.h>
#include <zlib.h>

// The blob opens with the size of what it unpacks to, so that the unpacking
// knows how much room to make.
#define M_SIZE_PREFIX 4

static void M_DumpAmmo(JSON_WRITE_IO *const io)
{
    JSONW_PUSH_OBJECT(io);
    for (int32_t i = 0; i < Gun_Registry_GetCount(); i++) {
        const WEAPON_INFO *const info = Gun_Registry_GetByIndex(i);
        if (info->save_ammo_key != nullptr) {
            JSONW_WRITE(io, info->save_ammo_key, Inv_GetAmmo(info->gun_type));
        }
    }
    JSONW_POP_AND_SET(io, "ammo");
}

static RESULT M_LoadAmmo(JSON_READ_IO *const io)
{
    MUST(JSON_PUSH(io, "ammo"));
    for (int32_t i = 0; i < Gun_Registry_GetCount(); i++) {
        const WEAPON_INFO *const info = Gun_Registry_GetByIndex(i);
        if (info->save_ammo_key == nullptr) {
            continue;
        }
        int32_t ammo = 0;
        if (Result_Absorb(JSON_READ(io, info->save_ammo_key, &ammo))) {
            Inv_SetAmmo(info->gun_type, ammo);
        }
    }
    MUST(JSON_POP(io));
    return OK;
}

bool Coop_Profile_Capture(void **const out_data, size_t *const out_size)
{
    const ITEM *const lara_item = Lara_GetItem();
    const GF_LEVEL *const level = Game_GetCurrentLevel();
    if (lara_item == nullptr || level == nullptr) {
        return false;
    }

    JSON_WRITE_IO *const io = JSON_WriteIO_Create();
    JSONW_WRITE(io, "level", level->num);
    JSONW_WRITE(io, "hit_points", lara_item->hit_points);
    JSONW_WRITE(io, "room_num", lara_item->room_num);
    JSONW_WRITE(io, "pos", lara_item->pos);
    JSONW_WRITE(io, "rot_y", lara_item->rot.y);
    SG_File_DumpInventory(io);
    M_DumpAmmo(io);

    void *bson = nullptr;
    size_t bson_size = 0;
    const bool written = SHOULD(
        BSON_Write(JSON_WriteIO_GetRoot(io), &bson, &bson_size),
        "Coop: cannot write a profile");
    JSON_WriteIO_Destroy(io);
    if (!written) {
        return false;
    }

    uLongf packed_size = compressBound(bson_size);
    uint8_t *const blob = Memory_Alloc(M_SIZE_PREFIX + packed_size);
    const bool packed = compress(
                            blob + M_SIZE_PREFIX, &packed_size,
                            (const Bytef *)bson, (uLongf)bson_size)
        == Z_OK;
    Memory_Free(bson);
    if (!packed || M_SIZE_PREFIX + packed_size > COOP_PROFILE_MAX_SIZE) {
        Memory_Free(blob);
        return false;
    }
    for (int32_t i = 0; i < M_SIZE_PREFIX; i++) {
        blob[i] = (bson_size >> (8 * i)) & 0xFF;
    }
    *out_data = blob;
    *out_size = M_SIZE_PREFIX + packed_size;
    return true;
}

// The profile's fields, or nullptr for a blob that does not unpack to them.
// The caller frees what comes back with JSON_ValueFree.
static JSON_VALUE *M_Unpack(const void *const data, const size_t size)
{
    if (data == nullptr || size <= M_SIZE_PREFIX
        || size > COOP_PROFILE_MAX_SIZE) {
        return nullptr;
    }
    const uint8_t *const bytes = data;
    uLongf unpacked_size = 0;
    for (int32_t i = 0; i < M_SIZE_PREFIX; i++) {
        unpacked_size |= (uLongf)bytes[i] << (8 * i);
    }
    // A profile unpacks to little; a size past any sense is damage.
    if (unpacked_size == 0 || unpacked_size > 64 * COOP_PROFILE_MAX_SIZE) {
        return nullptr;
    }

    char *const unpacked = Memory_Alloc(unpacked_size);
    const uLongf expected = unpacked_size;
    const bool ok = uncompress(
                        (Bytef *)unpacked, &unpacked_size,
                        bytes + M_SIZE_PREFIX, (uLongf)(size - M_SIZE_PREFIX))
            == Z_OK
        && unpacked_size == expected;
    JSON_VALUE *root = nullptr;
    if (!ok || !Result_Absorb(BSON_Parse(unpacked, unpacked_size, &root))) {
        root = nullptr;
    }
    Memory_Free(unpacked);
    return root;
}

int32_t Coop_Profile_GetLevelNum(const void *const data, const size_t size)
{
    JSON_VALUE *const root = M_Unpack(data, size);
    if (root == nullptr) {
        return -1;
    }
    JSON_READ_IO *const io =
        JSON_ReadIO_Create(root, SG_CURRENT_VERSION, "co-op profile");
    int32_t level_num = -1;
    if (!Result_Absorb(JSON_READ(io, "level", &level_num))) {
        level_num = -1;
    }
    JSON_ReadIO_Destroy(io);
    JSON_ValueFree(root);
    return level_num;
}

int16_t Coop_Profile_GetHitPoints(const void *const data, const size_t size)
{
    JSON_VALUE *const root = M_Unpack(data, size);
    if (root == nullptr) {
        return 0;
    }
    JSON_READ_IO *const io =
        JSON_ReadIO_Create(root, SG_CURRENT_VERSION, "co-op profile");
    int16_t hit_points = 0;
    if (!Result_Absorb(JSON_READ(io, "hit_points", &hit_points))) {
        hit_points = 0;
    }
    JSON_ReadIO_Destroy(io);
    JSON_ValueFree(root);
    return hit_points;
}

static RESULT M_Apply(
    JSON_READ_IO *const io, const bool with_kit, const bool with_position)
{
    ITEM *const lara_item = Lara_GetItem();
    if (lara_item == nullptr) {
        return FAIL("Lara is not in the level");
    }

    int32_t level_num = -1;
    int16_t hit_points = 0;
    MUST(JSON_READ(io, "level", &level_num));
    MUST(JSON_READ(io, "hit_points", &hit_points));
    if (with_kit) {
        MUST(SG_File_LoadInventory(io));
        MUST(M_LoadAmmo(io));
        if (hit_points > 0) {
            lara_item->hit_points = MIN(hit_points, LARA_MAX_HITPOINTS);
        }
    }

    const GF_LEVEL *const level = Game_GetCurrentLevel();
    if (!with_position || level == nullptr || level->num != level_num) {
        return OK;
    }
    int16_t room_num = NO_ROOM;
    XYZ_32 pos = {};
    int16_t rot_y = 0;
    MUST(JSON_READ(io, "room_num", &room_num));
    MUST(JSON_READ(io, "pos", &pos));
    MUST(JSON_READ(io, "rot_y", &rot_y));
    if (room_num < 0 || room_num >= Room_GetCount()) {
        return FAIL("the profile names room %d, which the level lacks",
            room_num);
    }
    // Where she stood may be taken now, by a door shut since; she stays put
    // then rather than land in a wall.
    if (Lara_Cheat_Teleport(pos, room_num)) {
        lara_item->rot.y = rot_y;
    }
    return OK;
}

bool Coop_Profile_Apply(
    const void *const data, const size_t size, const bool with_kit,
    const bool with_position)
{
    JSON_VALUE *const root = M_Unpack(data, size);
    if (root == nullptr) {
        LOG_WARNING("Coop: a profile came in damaged");
        return false;
    }
    JSON_READ_IO *const io =
        JSON_ReadIO_Create(root, SG_CURRENT_VERSION, "co-op profile");
    const bool applied =
        SHOULD(
            M_Apply(io, with_kit, with_position),
            "Coop: cannot apply a profile");
    JSON_ReadIO_Destroy(io);
    JSON_ValueFree(root);
    return applied;
}
