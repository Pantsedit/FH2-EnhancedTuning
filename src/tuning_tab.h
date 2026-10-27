#ifndef LIVETUNE_TUNING_TAB_H
#define LIVETUNE_TUNING_TAB_H
#include <stddef.h>

typedef void (*TabLog)(const char *message);

// Adds the Enhanced Tuning tab to the Tuning menu (first, or after Differential).
// Returns 1 when installed; 0 means nothing was changed and the reason was logged.
int tuning_tab_install(unsigned char *image, size_t image_size, int first, TabLog log);

#endif
