#pragma once

#include <trx/game/game_flow/types.h>
#include <trx/game/items/enum.h>
#include <trx/game/items/types.h>
#include <trx/game/objects/ids.h>

#include <stdbool.h>
#include <stdint.h>

// What the rest of the game knows of co-op: one call a frame, one reset a
// level, and a few questions.

// Whether this session plays co-op in any form: over the network, or with the
// local ghost.
bool Coop_IsActive(void);

// Whether this game joined a co-op session hosted elsewhere, so that the
// host's saves and level are the ones that count.
bool Coop_IsClient(void);

// Whether the current level is one a player plays. The demos replay recorded
// input against a world that must stay as recorded, and the title and
// cutscenes have nobody to show.
bool Coop_IsPlayableLevel(void);

// Called once per frame in every phase: menus and loading screens included,
// so the connection keeps going while a player is out of the level.
void Coop_Tick(void);

// Called once per logic frame of play, after Lara has moved.
void Coop_Control(void);

// Draw what co-op shows over the scene, such as the other players' names.
// Called after the rooms are drawn, while the game's own overlay is.
void Coop_DrawOverlay(void);

// In co-op the host runs the creatures. A client skips their control and
// shows them as the host says; the host points each one at the nearest
// player around its control.
bool Coop_SkipsItemControl(int16_t item_num);
void Coop_BeginItemControl(int16_t item_num);
void Coop_EndItemControl(void);

// Damage about to be dealt to `item`. Returns true where co-op deals with it
// instead: a puppet's blows go to its player, and a client's shots on a
// creature go to the host.
bool Coop_InterceptDamage(
    ITEM *item, int16_t damage, ITEM_DAMAGE_FLAGS flags, const ITEM *sender);

// A creature about to wake. Returns true where it is the host's to wake, as on
// a client, which tells the host instead.
bool Coop_InterceptActivation(int16_t item_num, bool force);

// Whether a dead Lara should wait to be revived rather than see the death
// screen.
bool Coop_ShouldHoldDeath(void);

// Lara reached the level's exit, and the level's statistics come next. There
// a player who has seen them waits for the others, the host's level going on
// behind them meanwhile.
void Coop_OnReachExit(void);
bool Coop_HoldsLevelStats(void);
void Coop_TickLevelStats(void);

// How the question of what a large medipack is for was answered.
typedef enum {
    // Nothing asked, or nothing answered yet.
    COOP_REVIVE_ANSWER_NONE,
    // The medipack is spent, on Lara or on a fallen player.
    COOP_REVIVE_ANSWER_SPENT,
    // The medipack stays unspent.
    COOP_REVIVE_ANSWER_KEPT,
} COOP_REVIVE_ANSWER;

// A large medipack was picked in the inventory. With a teammate down in the
// level, the player is asked whether to heal or to revive, and this returns
// true: the inventory waits for the answer, with the game still paused, and
// nothing is spent yet. Otherwise it returns false, for the medipack to heal
// as ever.
bool Coop_OfferRevive(OBJECT_ID object_id);
// Whether the question is waiting for an answer.
bool Coop_IsRevivePromptOpen(void);
// The answer once given, which is then forgotten.
COOP_REVIVE_ANSWER Coop_TakeReviveAnswer(void);
// The answers: keep the medipack, spend it on Lara as outside co-op, or on
// reviving the player numbered `number`, which keeps it where that cannot be.
void Coop_CloseRevivePrompt(void);
void Coop_HealWithMedipack(void);
bool Coop_ReviveWithMedipack(int32_t number);

// A level's sequence is starting, opening videos and all. The host tells the
// others to start it too.
void Coop_OnLevelSequence(const GF_LEVEL *level, GF_SEQUENCE_CONTEXT seq_ctx);

// Whether this game is joining a level the host has under way, without the
// videos, cutscenes and pictures that open it.
bool Coop_SkipsLevelIntro(void);

// The game is going back to the title, which a player who played leaves the
// session from.
void Coop_OnTitle(void);

// The level is about to unload, with Lara still in it.
void Coop_OnLevelEnding(void);

// Forget everything tied to the level. Its items and minted objects are about
// to go.
void Coop_Reset(void);

// Whether an item stands in for another player, which no savegame should
// carry.
bool Coop_IsPuppetItem(int16_t item_num);
