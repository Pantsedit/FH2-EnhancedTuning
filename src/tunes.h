#ifndef LIVETUNE_TUNES_H
#define LIVETUNE_TUNES_H
#include <stdint.h>
#include <wchar.h>

#include "tuning_slider.h"

struct FH2PluginDatabaseFrame;

// The Enhanced Tuning sliders, in page order. Each is a multiplier on the
// car's stock database values; 1.0 is stock. The exception is MW Handling (the
// last one), an intensity from 0 (stock) to 1 (full preset).
extern const SliderSpec tune_sliders[];
extern const unsigned tune_slider_count;

typedef void (*TuneRebuild)(uint32_t car);  // rebuild the driven car after its rows changed

// Plugin start: loads saved tunes from <plugins_directory>\..\tunes.ini.
void tunes_init(TabLog log, TuneRebuild rebuild, const wchar_t *plugins_directory);
// FH2PluginDatabaseUpdate: keeps the merged connection (used only on this
// thread), checks which columns exist and writes saved tunes.
void tunes_database(const struct FH2PluginDatabaseFrame *frame);
// FH2PluginVehicleUpdate: notes the driven car and writes pending tunes.
void tunes_apply(uint32_t car);
unsigned long tunes_database_thread(void);  // 0 until the database arrives

// Slider getter/setter for the driven car (any thread).
float tunes_get(unsigned index);
int tunes_set(unsigned index, float value);
// Slider value for any car by Data_Car Id (any thread, no database access).
// The slider's stock setting (1.0 for every multiplier) if the car has no tune.
float tunes_multiplier_for(uint32_t car, unsigned index);

#endif
