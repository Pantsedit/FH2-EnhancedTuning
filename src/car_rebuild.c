// Rebuild the driven car's physics from the database, in place.
//
// A same-car reload never rebuilds the car: unloading hides the Car object in
// the car cache (EntityModel::CarCacheModule, one hidden player car), and the
// next load is handed that same object back, since the cache key (model id,
// upgrades, a few flags) doesn't include database contents. Only a different
// model forces a fresh Car::Init. See docs/model_cache.md.
//
// Car vtable slot 0x48 (game+0xf75610, also ForzaCar's) is the game's own
// "apply upgrades" rebuild: (Car*, const uint8_t upgrades[0xB4], bool
// resetTune). It re-runs the physics builder (which reads Data_Car and the
// engine tables) and re-initialises the live CCarDynamics from the result.
// We pass the car's current upgrades (car+0x120) and resetTune 0, so only the
// database values change.
//
// Every address is checked before use; anything unexpected and nothing is called.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "car_rebuild.h"

enum {
    APPLY_UPGRADES = 0xf75610,  // Car vtable slot 0x48
    UPGRADES = 0x120,           // car+0x120: the 0xB4-byte upgrade block
    UPGRADES_SIZE = 0xB4,
    MODEL_ID = 0x118,           // car+0x118: Data_Car Id
};

static const unsigned char apply_entry[16] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56,
                                              0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41};

typedef void (*ApplyUpgradesFn)(void *car, const uint8_t *upgrades, uint8_t reset_tune);

static unsigned char *game;
static int entry_matches;

void car_rebuild_init(unsigned char *image) {
    game = image;
    entry_matches = !memcmp(image + APPLY_UPGRADES, apply_entry, sizeof apply_entry);
}

static int fail(TabLog log, const char *why) {
    char line[200];
    snprintf(line, sizeof line, "LIVETUNE: in-place rebuild skipped (%s); swap cars to apply.\r\n", why);
    log(line);
    return 0;
}

int car_rebuild(uint32_t car_id, void *car, TabLog log) {
    if (!game || !entry_matches) return fail(log, "game code didn't match");
    if (!car) return fail(log, "no car");
    SIZE_T got = 0;
    void **vtable = NULL;
    void *apply = NULL;
    uint32_t model = 0;
    _Alignas(16) uint8_t upgrades[UPGRADES_SIZE];
    if (!ReadProcessMemory(GetCurrentProcess(), (unsigned char *)car + MODEL_ID, &model, sizeof model, &got) || model != car_id)
        return fail(log, "the car isn't the tuned one");
    if (!ReadProcessMemory(GetCurrentProcess(), car, &vtable, sizeof vtable, &got) || !vtable ||
        !ReadProcessMemory(GetCurrentProcess(), vtable + 0x48 / 8, &apply, sizeof apply, &got) || apply != game + APPLY_UPGRADES)
        return fail(log, "car's slot 0x48 isn't the expected rebuild");
    if (!ReadProcessMemory(GetCurrentProcess(), (unsigned char *)car + UPGRADES, upgrades, sizeof upgrades, &got))
        return fail(log, "couldn't read the car's upgrades");
    ((ApplyUpgradesFn)apply)(car, upgrades, 0);
    char line[160];
    snprintf(line, sizeof line, "LIVETUNE: car %u rebuilt in place from the database.\r\n", car_id);
    log(line);
    return 1;
}
