#ifndef LIVETUNE_CAR_REPREPARE_H
#define LIVETUNE_CAR_REPREPARE_H
#include <stdint.h>

#include "tuning_tab.h"

// Checks the game functions used below (call once at startup).
void car_reprepare_init(unsigned char *image);

// Rebuilds the current career car from the database the way confirming it in
// My Cars does, if it is `car_id` (`car` is the live car object, used to reach
// its player). Game thread only. Returns 1 when done.
int car_reprepare(uint32_t car_id, void *car, TabLog log);

#endif
