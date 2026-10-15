#pragma once

#include <trx/core/vector.h>
#include <trx/game/shell/mod.h>

typedef struct {
    int32_t engine_version;

    bool check_lua_api;
    const SHELL_MOD *mod;
    // The game the player named on the command line, and whether they named
    // one at all. A named game that cannot be played is an error rather than
    // something to quietly pick around.
    const char *mod_request;
    bool mod_explicit;
    struct {
        int32_t num;
        const char *query;
        const char *path;
    } level_request;
    int32_t save_to_load;
} STARTUP_SETTINGS;

typedef struct {
    // Owned argv snapshot used as backing storage for pointer-valued fields
    // (e.g. level/replay/test paths). Freed by Shell_FreeArgs.
    VECTOR *original_args;

    STARTUP_SETTINGS startup;
    const char *test_record_path;
    const char *test_replay_path;
    bool headless;
    bool debug_render_performance;
    int32_t headless_fps; // in headless mode, force fixed fps (0 = unlocked)
    bool quiet;
    struct {
        // A puppet Lara replaying Lara's moves, a stand-in for a remote
        // player until there is a network to drive one.
        bool ghost;
        int32_t ghost_delay; // in logic frames (0 = default)
        // Play over the network: either host a game, or join the one hosted
        // at `join`, a name or an address.
        bool host;
        const char *join;
        int32_t port; // 0 = default
        // The name shown to the other players, which also picks the player
        // id kept for it.
        const char *name;
    } coop;
} SHELL_ARGS;

// Adopts `raw_args`.
// Requirements:
// - `raw_args` items must be `char *` allocated on heap (or nullptr).
// - ownership of vector + strings transfers to returned SHELL_ARGS.
// - caller must not free or mutate `raw_args` after this call.
RESULT Shell_ParseArgs(VECTOR *raw_args, SHELL_ARGS **out_args);
// Frees SHELL_ARGS and its adopted `original_args` vector + strings.
void Shell_FreeArgs(SHELL_ARGS *args);
