#include <trx/game/coop/common.h>

#include <trx/game/coop/creatures.h>
#include <trx/game/coop/ghost.h>
#include <trx/game/coop/net.h>
#include <trx/game/coop/puppet.h>
#include <trx/game/coop/world.h>
#include <trx/game/game.h>
#include <trx/game/game_flow.h>
#include <trx/game/inventory.h>
#include <trx/game/items.h>
#include <trx/game/lara.h>
#include <trx/game/objects.h>
#include <trx/game/sound.h>
#include <trx/game/stats.h>
#include <trx/game/shell.h>

bool Coop_IsActive(void)
{
    return Shell_GetArgs()->coop.ghost || Coop_Net_IsEnabled();
}

bool Coop_IsClient(void)
{
    return Coop_Net_IsEnabled() && !Coop_Net_IsHost();
}

bool Coop_IsPlayableLevel(void)
{
    const GF_LEVEL *const level = GF_GetCurrentLevel();
    if (level == nullptr) {
        return false;
    }
    switch (level->type) {
    case GFL_NORMAL:
    case GFL_GYM:
    case GFL_BONUS:
        return true;
    default:
        return false;
    }
}

void Coop_Tick(void)
{
    // Called even with no session, to start the one the command line asks
    // for.
    Coop_Net_Service();
}

void Coop_Control(void)
{
    if (!Coop_IsActive() || !Game_IsPlaying() || Lara_GetItem() == nullptr
        || !Coop_IsPlayableLevel()) {
        return;
    }
    // The ghost borrows the puppet the other player would have, so the two
    // never run together, and the network wins.
    if (Coop_Net_IsEnabled()) {
        Coop_Net_Control();
    } else {
        Coop_Ghost_Control();
    }
}

static bool M_IsNetLevel(void)
{
    return Coop_Net_IsEnabled() && Coop_IsPlayableLevel();
}

bool Coop_SkipsItemControl(const int16_t item_num)
{
    return Coop_IsClient() && Coop_IsPlayableLevel()
        && Coop_Creatures_IsShared(item_num);
}

void Coop_BeginItemControl(const int16_t item_num)
{
    if (M_IsNetLevel() && Coop_Net_IsHost()) {
        Coop_Creatures_BeginControl(item_num);
    }
}

void Coop_EndItemControl(void)
{
    Coop_Creatures_EndControl();
}

bool Coop_InterceptDamage(
    ITEM *const item, const int16_t damage, const ITEM_DAMAGE_FLAGS flags,
    const ITEM *const sender)
{
    if (!M_IsNetLevel()) {
        return false;
    }

    // Lara is out of the level, at its exit, while it goes on behind her.
    if (Coop_Net_IsHost() && Coop_Creatures_IsLaraAway()
        && Item_GetIndex(item) == Lara_GetLaraInfo()->item_num) {
        return true;
    }

    // A puppet's player is the one to suffer what it takes, in its own game.
    const int32_t slot = Coop_Puppet_GetSlot(Item_GetIndex(item));
    if (slot >= 0) {
        if (Coop_Net_IsHost() && item->hit_points > 0
            && (damage > 0 || (flags & IDF_FATAL) != 0)) {
            Coop_Net_SendDamage(slot, damage, (uint8_t)flags);
        }
        return true;
    }

    // The host's creatures take only the host's word, which takes in what
    // Lara here lands on them.
    if (Coop_IsClient() && Coop_Creatures_IsShared(Item_GetIndex(item))) {
        if (sender == Lara_GetItem() && damage > 0
            && Item_CanTakeDamage(item)) {
            Coop_Net_SendHit(Item_GetIndex(item), damage, (uint8_t)flags);
        }
        return true;
    }
    return false;
}

bool Coop_InterceptActivation(const int16_t item_num, const bool force)
{
    if (!Coop_IsClient() || !Coop_IsPlayableLevel() || Game_IsSettingUpItems()
        || !Coop_Creatures_IsShared(item_num)) {
        return false;
    }
    Coop_Net_SendActivate(item_num, force);
    return true;
}

void Coop_DrawOverlay(void)
{
    if (Coop_IsActive() && Coop_IsPlayableLevel()) {
        Coop_Puppet_DrawLabels();
    }
}

static bool m_RevivePrompt = false;
static COOP_REVIVE_ANSWER m_ReviveAnswer = COOP_REVIVE_ANSWER_NONE;

bool Coop_ShouldHoldDeath(void)
{
    return Coop_Net_IsEnabled() && Coop_IsPlayableLevel()
        && Coop_Net_ShouldHoldDeath();
}

void Coop_OnReachExit(void)
{
    if (M_IsNetLevel()) {
        Coop_Net_OnReachExit();
    }
}

bool Coop_HoldsLevelStats(void)
{
    return M_IsNetLevel() && Coop_Net_HoldsLevelStats();
}

void Coop_TickLevelStats(void)
{
    if (M_IsNetLevel()) {
        Coop_Net_TickLevelStats();
    }
}

bool Coop_OfferRevive(const OBJECT_ID object_id)
{
    if ((object_id != O_LARGE_MEDIPACK_OPTION
         && object_id != O_LARGE_MEDIPACK_ITEM)
        || !Coop_Net_IsEnabled() || !Coop_IsPlayableLevel()) {
        return false;
    }
    COOP_PLAYER_INFO dead[COOP_MAX_PLAYERS];
    if (Coop_Net_GetDeadTeammates(dead) == 0) {
        return false;
    }
    m_RevivePrompt = true;
    m_ReviveAnswer = COOP_REVIVE_ANSWER_NONE;
    return true;
}

bool Coop_IsRevivePromptOpen(void)
{
    return m_RevivePrompt;
}

COOP_REVIVE_ANSWER Coop_TakeReviveAnswer(void)
{
    const COOP_REVIVE_ANSWER answer = m_ReviveAnswer;
    m_ReviveAnswer = COOP_REVIVE_ANSWER_NONE;
    return answer;
}

static void M_Answer(const COOP_REVIVE_ANSWER answer)
{
    m_RevivePrompt = false;
    m_ReviveAnswer = answer;
}

void Coop_CloseRevivePrompt(void)
{
    if (m_RevivePrompt) {
        M_Answer(COOP_REVIVE_ANSWER_KEPT);
    }
}

void Coop_HealWithMedipack(void)
{
    Lara_UseItem(O_LARGE_MEDIPACK_OPTION);
    M_Answer(COOP_REVIVE_ANSWER_SPENT);
}

bool Coop_ReviveWithMedipack(const int32_t number)
{
    if (!Inv_HasItem(O_LARGE_MEDIPACK_ITEM) || !Coop_Net_Revive(number)) {
        M_Answer(COOP_REVIVE_ANSWER_KEPT);
        return false;
    }
    Inv_RemoveItem(O_LARGE_MEDIPACK_ITEM);
    Sound_Effect(SFX_MENU_MEDI, nullptr, SPM_ALWAYS);
    Stats_AddMedipacksUsed(1);
    M_Answer(COOP_REVIVE_ANSWER_SPENT);
    return true;
}

void Coop_OnLevelSequence(
    const GF_LEVEL *const level, const GF_SEQUENCE_CONTEXT seq_ctx)
{
    switch (level->type) {
    case GFL_NORMAL:
    case GFL_GYM:
    case GFL_BONUS:
        Coop_Net_OnLevelSequence(level->num, seq_ctx != GFSC_SAVED);
        break;
    default:
        break;
    }
}

bool Coop_SkipsLevelIntro(void)
{
    return Coop_Net_SkipsIntro();
}

void Coop_OnTitle(void)
{
    Coop_Net_OnTitle();
}

void Coop_OnLevelEnding(void)
{
    if (Coop_Net_IsEnabled() && Coop_IsPlayableLevel()
        && Lara_GetItem() != nullptr) {
        Coop_Net_OnLevelEnding();
    }
}

void Coop_Reset(void)
{
    m_RevivePrompt = false;
    m_ReviveAnswer = COOP_REVIVE_ANSWER_NONE;
    Coop_Creatures_EndControl();
    Coop_Creatures_Reset();
    Coop_Ghost_Reset();
    Coop_Puppet_Reset();
    Coop_World_Reset();
    Coop_Net_OnLevelUnload();
}

bool Coop_IsPuppetItem(const int16_t item_num)
{
    return Coop_Puppet_IsItem(item_num);
}
