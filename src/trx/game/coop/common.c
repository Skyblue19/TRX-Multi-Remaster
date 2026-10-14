#include <trx/game/coop/common.h>

#include <trx/game/coop/ghost.h>
#include <trx/game/coop/net.h>
#include <trx/game/coop/puppet.h>
#include <trx/game/game.h>
#include <trx/game/lara.h>
#include <trx/game/shell.h>

bool Coop_IsActive(void)
{
    return Shell_GetArgs()->coop.ghost || Coop_Net_IsEnabled();
}

void Coop_Control(void)
{
    if (!Coop_IsActive() || !Game_IsPlaying() || Lara_GetItem() == nullptr) {
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

void Coop_Reset(void)
{
    Coop_Ghost_Reset();
    Coop_Puppet_Reset();
}

bool Coop_IsPuppetItem(const int16_t item_num)
{
    return Coop_Puppet_IsItem(item_num);
}
