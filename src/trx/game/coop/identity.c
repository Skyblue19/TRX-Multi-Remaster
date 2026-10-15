#include <trx/game/coop/identity.h>

#include <trx/core/file.h>
#include <trx/core/log.h>
#include <trx/core/memory.h>
#include <trx/core/result.h>
#include <trx/core/strings.h>
#include <trx/core/utils.h>
#include <trx/game/paths.h>
#include <trx/game/shell.h>

#include <SDL2/SDL_timer.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define M_DEFAULT_NAME "Lara"
#define M_HEX_SIZE (COOP_ID_SIZE * 2)
// The name last chosen on its first line, the host last joined on its second.
#define M_PROFILE_FILE "coop_profile.txt"
#define M_PROFILE_MAX_SIZE 256

static COOP_IDENTITY m_Identity;
static char m_LastAddress[COOP_ADDRESS_SIZE];
static bool m_IsLoaded = false;

static bool M_IsNameChar(const char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool Coop_Identity_CleanName(const char *const in, char out[COOP_NAME_SIZE])
{
    int32_t len = 0;
    for (const char *c = in; c != nullptr && *c != '\0'; c++) {
        if (M_IsNameChar(*c) && len < COOP_NAME_SIZE - 1) {
            out[len++] = *c;
        }
    }
    out[len] = '\0';
    return len > 0;
}

// An address keeps what a host name or an IP address may hold.
static void M_CleanAddress(const char *const in, char out[COOP_ADDRESS_SIZE])
{
    int32_t len = 0;
    for (const char *c = in; c != nullptr && *c != '\0'; c++) {
        if ((M_IsNameChar(*c) || *c == '.' || *c == ':')
            && len < COOP_ADDRESS_SIZE - 1) {
            out[len++] = *c;
        }
    }
    out[len] = '\0';
}

// Nothing here has to be secret, only unlike any other player's id. The game
// keeps its own random sequences for play, and drawing from them would change
// what the creatures do, so the id is mixed from clocks instead.
static uint64_t M_SplitMix(uint64_t *const state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void M_MakeId(uint8_t id[COOP_ID_SIZE])
{
    const uint64_t here = 0;
    uint64_t state = SDL_GetPerformanceCounter() ^ ((uint64_t)time(nullptr) << 20)
        ^ (uint64_t)(uintptr_t)&here;
    for (int32_t i = 0; i < COOP_ID_SIZE; i += 8) {
        const uint64_t bits = M_SplitMix(&state);
        for (int32_t j = 0; j < 8; j++) {
            id[i + j] = (bits >> (8 * j)) & 0xFF;
        }
    }
}

static bool M_ParseHex(const char *const hex, uint8_t id[COOP_ID_SIZE])
{
    for (int32_t i = 0; i < COOP_ID_SIZE; i++) {
        unsigned int byte = 0;
        if (sscanf(&hex[i * 2], "%2x", &byte) != 1) {
            return false;
        }
        id[i] = (uint8_t)byte;
    }
    return true;
}

static char *M_GetConfigPath(const char *const file_name)
{
    return GamePath_Join(GAME_PATH_CONFIG_DIR, file_name);
}

// Reads a small text file whole, or returns false where there is none.
static bool M_ReadText(
    const char *const file_name, char *const out, const size_t out_size)
{
    char *const path = M_GetConfigPath(file_name);
    TRX_FILE *file = nullptr;
    const bool opened = IS_OK(File_OpenPath(path, FILE_OPEN_READ, &file));
    Memory_Free(path);
    if (!opened) {
        return false;
    }
    const size_t size = MIN(File_Size(file), out_size - 1);
    const bool read = File_TryReadData(file, out, size);
    File_Close(file);
    out[read ? size : 0] = '\0';
    return read;
}

static void M_WriteText(const char *const file_name, const char *const text)
{
    char *const path = M_GetConfigPath(file_name);
    TRX_FILE *file = nullptr;
    if (SHOULD(
            File_OpenPath(path, FILE_OPEN_WRITE, &file),
            "Coop: cannot keep the player's profile")) {
        File_WriteData(file, text, strlen(text));
        File_Close(file);
    }
    Memory_Free(path);
}

static void M_LoadId(void)
{
    char *const file_name =
        String_Format("coop_player_%s.id", m_Identity.name);
    char hex[M_HEX_SIZE + 2] = {};
    if (!M_ReadText(file_name, hex, sizeof(hex))
        || !M_ParseHex(hex, m_Identity.id)) {
        M_MakeId(m_Identity.id);
        for (int32_t i = 0; i < COOP_ID_SIZE; i++) {
            snprintf(&hex[i * 2], 3, "%02x", m_Identity.id[i]);
        }
        hex[M_HEX_SIZE] = '\n';
        hex[M_HEX_SIZE + 1] = '\0';
        M_WriteText(file_name, hex);
        LOG_INFO("Coop: made a new player id for %s", m_Identity.name);
    }
    Memory_Free(file_name);
}

static void M_SaveProfile(void)
{
    char *const text =
        String_Format("%s\n%s\n", m_Identity.name, m_LastAddress);
    M_WriteText(M_PROFILE_FILE, text);
    Memory_Free(text);
}

static void M_Load(void)
{
    char profile[M_PROFILE_MAX_SIZE] = {};
    char saved_name[COOP_NAME_SIZE] = {};
    if (M_ReadText(M_PROFILE_FILE, profile, sizeof(profile))) {
        char *const newline = strchr(profile, '\n');
        if (newline != nullptr) {
            *newline = '\0';
            M_CleanAddress(newline + 1, m_LastAddress);
        }
        Coop_Identity_CleanName(profile, saved_name);
    }

    if (!Coop_Identity_CleanName(
            Shell_GetArgs()->coop.name, m_Identity.name)) {
        strcpy(
            m_Identity.name,
            saved_name[0] != '\0' ? saved_name : M_DEFAULT_NAME);
    }
    M_LoadId();
}

static void M_EnsureLoaded(void)
{
    if (!m_IsLoaded) {
        M_Load();
        m_IsLoaded = true;
    }
}

const COOP_IDENTITY *Coop_Identity_Get(void)
{
    M_EnsureLoaded();
    return &m_Identity;
}

bool Coop_Identity_SetName(const char *const name)
{
    M_EnsureLoaded();
    char cleaned[COOP_NAME_SIZE];
    if (!Coop_Identity_CleanName(name, cleaned)) {
        return false;
    }
    if (strcmp(cleaned, m_Identity.name) != 0) {
        strcpy(m_Identity.name, cleaned);
        M_LoadId();
    }
    M_SaveProfile();
    return true;
}

const char *Coop_Identity_GetLastAddress(void)
{
    M_EnsureLoaded();
    return m_LastAddress;
}

void Coop_Identity_SetLastAddress(const char *const address)
{
    M_EnsureLoaded();
    M_CleanAddress(address, m_LastAddress);
    M_SaveProfile();
}
