#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A player's own part of a game: what Lara carries, her ammo and health, and
// where she stands. Each client sends its profile to the host, which keeps it
// in its saves and hands it back, so that a client goes on as it was.
//
// A profile travels as a blob: the fields as BSON, compressed, the way a
// savegame is.

// Bigger than any profile, which runs to a few hundred bytes.
#define COOP_PROFILE_MAX_SIZE 16384

// Lara's profile now, into a buffer the caller frees with Memory_Free.
// Returns false out of play.
bool Coop_Profile_Capture(void **out_data, size_t *out_size);

// Which level a profile was taken in, or -1 for a damaged one.
int32_t Coop_Profile_GetLevelNum(const void *data, size_t size);
// Lara's health when a profile was taken, or 0 for a damaged one.
int16_t Coop_Profile_GetHitPoints(const void *data, size_t size);

// Make Lara carry what the profile says. With `with_position`, she is moved
// to where it was taken too, so long as that was in this level. Returns false
// for a damaged profile, leaving Lara as she was.
bool Coop_Profile_Apply(const void *data, size_t size, bool with_position);
