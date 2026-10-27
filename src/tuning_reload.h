#ifndef LIVETUNE_TUNING_RELOAD_H
#define LIVETUNE_TUNING_RELOAD_H
#include <stddef.h>

#include "tuning_tab.h"

// Leaving the pause menu's Tuning screen reloads the car (via Extra Options'
// Get in Car sequence). Needs that feature on; returns 1 when chained.
int tuning_reload_install(unsigned char *image, size_t image_size, TabLog log, int get_in_car_enabled);

// The pause.xml edit on its own (for testing): new length, or 0 to leave it.
size_t tuning_reload_edit(const char *text, size_t length, char *out, size_t capacity);

#endif
