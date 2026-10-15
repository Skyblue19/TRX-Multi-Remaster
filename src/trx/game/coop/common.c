#include <trx/game/coop/common.h>

#include <trx/game/coop/ghost.h>
#include <trx/game/coop/net.h>
#include <trx/game/coop/puppet.h>
#include <trx/game/coop/world.h>
#include <trx/game/game.h>
#include <trx/game/game_flow.h>
#include <trx/game/lara.h>
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

void Coop_DrawOverlay(void)
{
    if (Coop_IsActive() && Coop_IsPlayableLevel()) {
        Coop_Puppet_DrawLabels();
    }
}

void Coop_OnLevelSequence(const GF_LEVEL *const level)
{
    switch (level->type) {
    case GFL_NORMAL:
    case GFL_GYM:
    case GFL_BONUS:
        Coop_Net_OnLevelSequence(level->num);
        break;
    default:
        break;
    }
}

void Coop_Reset(void)
{
    Coop_Ghost_Reset();
    Coop_Puppet_Reset();
    Coop_World_Reset();
    Coop_Net_OnLevelUnload();
}

bool Coop_IsPuppetItem(const int16_t item_num)
{
    return Coop_Puppet_IsItem(item_num);
}
