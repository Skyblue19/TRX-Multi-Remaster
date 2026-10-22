#include <trx/game/phase/phase_coop_wait.h>

#include <trx/core/memory.h>
#include <trx/game/console.h>
#include <trx/game/coop/common.h>
#include <trx/game/input.h>
#include <trx/game/output.h>
#include <trx/game/shell.h>

// How often the player waiting is reminded what for.
#define M_REMINDER_FRAMES 150

typedef struct {
    // The level's loading picture, or nullptr to wait on black.
    const char *picture;
    int32_t reminder_wait;
} M_PRIV;

static const char *M_GetLoadingPicture(const GF_LEVEL *const level)
{
    const GF_SEQUENCE *const sequence = &level->sequence;
    for (int32_t i = 0; i < sequence->length; i++) {
        const GF_SEQUENCE_EVENT *const event = &sequence->events[i];
        if (event->type == GFS_LOADING_SCREEN && event->data != nullptr) {
            const GF_DISPLAY_PICTURE_DATA *const data = event->data;
            return data->path;
        }
    }
    return nullptr;
}

static PHASE_CONTROL M_Start(PHASE *const phase)
{
    M_PRIV *const p = phase->priv;
    if (p->picture != nullptr && !Output_Overlay_LoadImage(p->picture)) {
        p->picture = nullptr;
    }
    return (PHASE_CONTROL) { .action = PHASE_ACTION_CONTINUE };
}

static void M_End(PHASE *const phase)
{
}

static PHASE_CONTROL M_Control(PHASE *const phase)
{
    M_PRIV *const p = phase->priv;
    Input_Update();
    Shell_ProcessInput();

    // A player tired of waiting goes back to the title, which leaves the
    // game.
    if (g_InputDB.menu_back) {
        return (PHASE_CONTROL) {
            .action = PHASE_ACTION_END,
            .gf_cmd = { .action = GF_EXIT_TO_TITLE },
        };
    }

    if (!Coop_TickWaitForHost()) {
        return (PHASE_CONTROL) {
            .action = PHASE_ACTION_END,
            .gf_cmd = { .action = GF_NOOP },
        };
    }

    if (p->reminder_wait-- <= 0) {
        p->reminder_wait = M_REMINDER_FRAMES;
        Console_Info("Coop: waiting for the host (Esc to leave)");
    }
    return (PHASE_CONTROL) { .action = PHASE_ACTION_CONTINUE };
}

static void M_Draw(PHASE *const phase)
{
    const M_PRIV *const p = phase->priv;
    if (p->picture != nullptr) {
        Output_Overlay_DrawImage(p->picture);
    } else {
        Output_Overlay_DrawBlackRectangle(1.0f);
    }
    Output_Flush();
}

PHASE *Phase_CoopWait_Create(const GF_LEVEL *const level)
{
    PHASE *const phase = Memory_Alloc(sizeof(PHASE));
    M_PRIV *const p = Memory_Alloc(sizeof(M_PRIV));
    p->picture = M_GetLoadingPicture(level);
    p->reminder_wait = 0;
    phase->priv = p;
    phase->start = M_Start;
    phase->end = M_End;
    phase->control = M_Control;
    phase->draw = M_Draw;
    phase->request_fade_to_black = nullptr;
    phase->uses_cross_fade_in = nullptr;
    return phase;
}

void Phase_CoopWait_Destroy(PHASE *const phase)
{
    Memory_Free(phase->priv);
    Memory_Free(phase);
}
