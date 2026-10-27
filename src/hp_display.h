#ifndef LIVETUNE_HP_DISPLAY_H
#define LIVETUNE_HP_DISPLAY_H
#include <stddef.h>
#include <wchar.h>

#include "tuning_tab.h"

// Display-only: scales the power and torque that the car-stat screens (car
// select / My Cars, dealers, upgrades, tuning load/save) show by the car's
// Horsepower multiplier. Only the UI's stat view model is changed; the
// CCarInGarage, the garage row and PI are untouched. See docs/hp_display.md.
// LiveTune.ini [Display] ScalePower=0 turns it off. Call once at startup.
int hp_display_init(unsigned char *image, size_t image_size, TabLog log, const wchar_t *plugins_directory);

#endif
