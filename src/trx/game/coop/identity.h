#pragma once

#include <stdbool.h>
#include <stdint.h>

// Who this player is, the same from one session to the next: a name to show,
// and a random id made the first time that name is used and kept in the
// config directory. The host tells its players apart by the id, never by the
// order they connect in. Each name has an id of its own, so that two players
// can share one game directory; a player who changes name is someone new to
// the host.

#define COOP_ID_SIZE 16
// Room for the longest name and its terminator.
#define COOP_NAME_SIZE 16
// Room for a host name or address and its terminator.
#define COOP_ADDRESS_SIZE 64

typedef struct {
    uint8_t id[COOP_ID_SIZE];
    char name[COOP_NAME_SIZE];
} COOP_IDENTITY;

// This player's identity. The name is --coop-name if given, else the one
// last chosen, else a default.
const COOP_IDENTITY *Coop_Identity_Get(void);

// Change this player's name, which is kept for next time. Returns false, and
// changes nothing, for a name left empty once cleaned.
bool Coop_Identity_SetName(const char *name);

// The host this player last joined, kept for next time, or "" for none.
const char *Coop_Identity_GetLastAddress(void);
void Coop_Identity_SetLastAddress(const char *address);

// Copy a name that came from elsewhere, keeping only what a name may hold.
// Returns false for a name left empty.
bool Coop_Identity_CleanName(const char *in, char out[COOP_NAME_SIZE]);
