#ifndef LIVETUNE_TUNING_SLIDER_H
#define LIVETUNE_TUNING_SLIDER_H
#include <stddef.h>

#include "tuning_tab.h"

enum { MAX_SLIDERS = 16 };

// One Enhanced Tuning slider. tuning_page.c builds the XAML row from the same
// entry, so the page's range always matches what the setter accepts.
typedef struct {
    const char *property;  // native property the XAML binds to
    const char *group;     // header row title; a new header starts when it changes
    const char *label;     // row label
    const char *help;      // help text shown for the row (plain text)
    float min, max, step;  // accepted range; values snap to step
    float small_change, large_change;  // how far the slider itself moves
    float stock;           // the setting that leaves the car stock (the page's grey marker)
} SliderSpec;

typedef float (*SliderGet)(unsigned index);
typedef int (*SliderSet)(unsigned index, float value);  // nonzero when accepted

// Registers one float property per slider on the Tuning page's types.
// Returns 1 when all were added.
int tuning_slider_install(unsigned char *image, size_t image_size, TabLog log, const SliderSpec *sliders,
                          unsigned count, SliderGet get, SliderSet set);

#endif
