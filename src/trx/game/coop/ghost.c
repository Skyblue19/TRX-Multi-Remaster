#include <trx/game/coop/ghost.h>

#include <trx/core/utils.h>
#include <trx/game/coop/puppet.h>
#include <trx/game/shell.h>

// A power of two, so the ring index wraps with a mask. At 30 frames a second
// it holds a little over eight seconds of Lara.
#define M_HISTORY_SIZE 256
#define M_DEFAULT_DELAY 30
// The ghost has the network's puppets to itself, as the two never run
// together.
#define M_SLOT 0

static struct {
    COOP_PLAYER_STATE history[M_HISTORY_SIZE];
    int32_t recorded;
} m_Ghost;

static int32_t M_GetDelay(void)
{
    int32_t delay = Shell_GetArgs()->coop.ghost_delay;
    if (delay <= 0) {
        return M_DEFAULT_DELAY;
    }
    CLAMP(delay, 1, M_HISTORY_SIZE - 1);
    return delay;
}

void Coop_Ghost_Control(void)
{
    m_Ghost.history[m_Ghost.recorded & (M_HISTORY_SIZE - 1)] =
        Coop_Puppet_CaptureLara();
    m_Ghost.recorded++;

    const int32_t delay = M_GetDelay();
    if (m_Ghost.recorded <= delay) {
        return;
    }
    const int32_t replayed = m_Ghost.recorded - 1 - delay;
    Coop_Puppet_SetName(M_SLOT, "Ghost");
    Coop_Puppet_Apply(
        M_SLOT, &m_Ghost.history[replayed & (M_HISTORY_SIZE - 1)]);
}

void Coop_Ghost_Reset(void)
{
    m_Ghost.recorded = 0;
}
