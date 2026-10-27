#ifndef LIVETUNE_CAR_REBUILD_H
#define LIVETUNE_CAR_REBUILD_H
#include <stdint.h>

#include "tuning_tab.h"

// Checks the game function used below (call once at startup).
void car_rebuild_init(unsigned char *image);

// Rebuilds the live car's physics from the database through the game's own
// upgrade-apply (Car vtable slot 0x48), keeping its upgrades and tune. `car` is
// the vehicle-update car pointer and must be `car_id`. Game thread only.
// Returns 1 when done.
int car_rebuild(uint32_t car_id, void *car, TabLog log);

#endif
