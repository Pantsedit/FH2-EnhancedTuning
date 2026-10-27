// Re-prepare the current career car from the database, as confirming it again
// in My Cars does, so the next reload spawns it with LiveTune's values.
//
// The garage caches the current career car (CGarage+0x50, a CCarInGarage). A
// same-car reload keeps that cache, which is why tunes needed a car swap.
// CGarage::SetCareerCar (game+0x2de4e0) drops the cache and rebuilds it from
// the database (no early-out for the current id), then
// ForzaLocalPlayerChoices::SetGarageCar (vtable slot 0x1a8, game+0x2a0c30)
// copies the fresh car into what LoadPlayerCar spawns from. That pair is what
// the car-select screen runs on "Get in Car". See docs/swap.md.
//
// Safe for save data: the rebuild happens in a new object with no garage-row
// id, so nothing is written to the saved garage row; the only setting written
// is "Main/CareerCar", rewritten with its current value.
//
// Every address is checked before use; anything unexpected and nothing is called.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "car_reprepare.h"

enum {
    GET_PLAYER_MANAGER = 0x327270,  // mov rax, [player manager]; ret
    SET_CAREER_CAR = 0x2de4e0,      // (garage, garage row id, Extra* consumed)
    SET_GARAGE_CAR = 0x2a0c30,      // choices vtable slot 0x1a8: (choices, garage, row id)
    SET_GARAGE_CAR_LOCAL = 0x2a0dc0, // the same slot in the local player's choices class (vtable game+0x2c5de38)
    EXTRA_CTOR = 0x215ec0,          // (Extra*), at least 0x60 bytes
    CAREER_CAR_ID = 0x2dcab0,       // uint32 (garage): "Main/CareerCar"
    LOCAL_CHOICES = 0xced1e0,       // local player 0 -> vf 0x100: its ForzaLocalPlayerChoices
    LOCAL_GARAGE = 0xced510,        // local player 0 -> vf 0xd8: its CGarage
};

static const struct {
    uint32_t rva;
    unsigned char bytes[8];
} entries[] = {
    {GET_PLAYER_MANAGER, {0x48, 0x8b, 0x05, 0xa1, 0xcb, 0x18, 0x03, 0xc3}},
    {SET_CAREER_CAR, {0x4c, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57}},
    {SET_GARAGE_CAR, {0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0x80}},
    {SET_GARAGE_CAR_LOCAL, {0x48, 0x8b, 0xc4, 0x41, 0x56, 0x48, 0x81, 0xec}},
    {EXTRA_CTOR, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83}},
    {CAREER_CAR_ID, {0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7}},
    {LOCAL_CHOICES, {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7}},
    {LOCAL_CHOICES + 0x1d, {0x48, 0x8b, 0x01, 0xff, 0x90, 0x00, 0x01, 0x00}},  // call [rax+0x100]
    {LOCAL_GARAGE, {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7}},
    {LOCAL_GARAGE + 0x1d, {0x48, 0x8b, 0x01, 0xff, 0x90, 0xd8, 0x00, 0x00}},   // call [rax+0xd8]
};

typedef void *(*NoArgFn)(void);
typedef void *(*ObjFn)(void *);
typedef uint32_t (*IdFn)(void *);
typedef void (*SetCareerCarFn)(void *, uint32_t, void *);
typedef void (*SetGarageCarFn)(void *, void *, uint32_t);

static unsigned char *game;
static int entries_match;

static int read_ptr(const void *at, void *out) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), at, out, sizeof(void *), &got) && got == sizeof(void *);
}

void car_reprepare_init(unsigned char *image) {
    game = image;
    entries_match = 1;
    for (unsigned i = 0; i < ARRAYSIZE(entries); ++i)
        if (memcmp(image + entries[i].rva, entries[i].bytes, sizeof entries[i].bytes)) entries_match = 0;
}

static int fail(TabLog log, const char *why) {
    char line[200];
    snprintf(line, sizeof line, "LIVETUNE: car re-prepare skipped (%s); swap cars to apply.\r\n", why);
    log(line);
    return 0;
}

int car_reprepare(uint32_t car_id, void *car, TabLog log) {
    char line[220];
    (void)car;
    if (!game || !entries_match) return fail(log, "game code didn't match");
    // The same getters the car-select screen uses for local player 0 (the live
    // car's own player pointer leads to a different choices object).
    void *garage = ((NoArgFn)(game + LOCAL_GARAGE))();
    void *choices = ((NoArgFn)(game + LOCAL_CHOICES))();
    void **choices_vtable = NULL;
    void *set_garage_car = NULL;
    if (!garage) return fail(log, "no garage");
    if (!choices || !read_ptr(choices, &choices_vtable) || !choices_vtable) return fail(log, "no player choices");
    if (!read_ptr(choices_vtable + 0x1a8 / 8, &set_garage_car) || (set_garage_car != game + SET_GARAGE_CAR && set_garage_car != game + SET_GARAGE_CAR_LOCAL)) {
        snprintf(line, sizeof line, "choices' slot 0x1a8 is %p, game+%llx, vtable game+%llx", set_garage_car,
                 (unsigned long long)((unsigned char *)set_garage_car - game),
                 (unsigned long long)((unsigned char *)choices_vtable - game));
        return fail(log, line);
    }
    // The cached career car must be the car we tuned.
    uint32_t row = ((IdFn)(game + CAREER_CAR_ID))(garage);
    void *cached = NULL;
    uint32_t cached_car = 0;
    SIZE_T got = 0;
    if (!row || !read_ptr((unsigned char *)garage + 0x50, &cached) || !cached ||
        !ReadProcessMemory(GetCurrentProcess(), (unsigned char *)cached + 0x40, &cached_car, sizeof cached_car, &got) ||
        cached_car != car_id) {
        snprintf(line, sizeof line, "LIVETUNE: current career car (row %u, car %u) isn't car %u; swap cars to apply.\r\n",
                 row, cached_car, car_id);
        log(line);
        return 0;
    }

    // Same calls, same order, as confirming the car in My Cars.
    _Alignas(16) unsigned char extra[0x80];
    memset(extra, 0, sizeof extra);
    ((ObjFn)(game + EXTRA_CTOR))(extra);
    ((SetCareerCarFn)(game + SET_CAREER_CAR))(garage, row, extra);  // consumes extra
    ((SetGarageCarFn)set_garage_car)(choices, garage, row);
    // SetGarageCar sets the "car changed" byte (choices+0x264, next to the row
    // id at +0x260) only when the row id differs, and without it the next reload
    // keeps the old car. Set it as a car swap would, so the reload builds the
    // re-prepared car.
    uint32_t choices_row = 0;
    unsigned char changed = 1;
    if (!ReadProcessMemory(GetCurrentProcess(), (unsigned char *)choices + 0x260, &choices_row, sizeof choices_row, &got) ||
        choices_row != row)
        return fail(log, "choices' row id didn't take the garage row");
    if (!WriteProcessMemory(GetCurrentProcess(), (unsigned char *)choices + 0x264, &changed, 1, &got))
        return fail(log, "couldn't mark the car as changed");
    snprintf(line, sizeof line, "LIVETUNE: car %u re-prepared from the database (garage row %u) and marked changed; the next reload spawns it.\r\n",
             car_id, row);
    log(line);
    return 1;
}
