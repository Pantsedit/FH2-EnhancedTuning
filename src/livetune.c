// Enhanced Tuning (LiveTune): an Extra Options plugin that adds an ENHANCED
// TUNING tab to Forza Horizon 2's Tuning screen.
//
//   tunes.c          the sliders and their edits to the merged game database
//   tuning_tab.c     the extra tab in the Tuning screen's tab table
//   tuning_slider.c  the native UI properties the sliders bind to
//   tuning_page.c    the tab's panel, inserted into the Tuning screen as it loads
//   ui_badge.c       the pause menu's Tune Car tile retitled "ENHANCED TUNING"
//   tuning_reload.c  leaving Tuning reloads the car (Extra Options' Get in Car)
//   car_reprepare.c  re-prepares the garage's copy of the car after a change
//   car_rebuild.c    rebuilds the live car in place (Car vtable slot 0x48)
//   hp_display.c     stat screens show the tuned horsepower (display only)
//
// Everything is in memory; no game file is replaced. Each patch first checks
// the exact bytes it expects and does nothing (with a log line) otherwise.
// Settings: LiveTune.ini beside the plugins folder. Tunes: tunes.ini, same place.

#include "fh2_plugin.h"
#include "car_rebuild.h"
#include "car_reprepare.h"
#include "hp_display.h"
#include "tuning_page.h"
#include "tuning_reload.h"
#include "tuning_slider.h"
#include "tuning_tab.h"
#include "tunes.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define ENHANCED_TUNING_VERSION "1.0.0"

static const FH2PluginContext *context;

static void log_line(const char *text) {
    if (context && context->log_utf8) context->log_utf8(text);
}

static int read_bytes(const void *from, void *to, SIZE_T size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), from, to, size, &got) && got == size;
}

// ---- Tunes ----------------------------------------------------------------
// The Enhanced Tuning sliders and their database writes live in tunes.c. This
// file only hands it the database, the driven car, and the rebuild below.

static volatile uint32_t current_car;  // last car seen in a vehicle update

// A reload of the same car reuses the garage's cached career car; only
// selecting a car rebuilds it. So after a database edit, re-prepare the current
// career car the way confirming it in My Cars does (car_reprepare.c); the next
// reload (leaving Tuning) spawns it with the new values.
//
// Never rebuild the live car's setup record directly (game+0x2cfa50 or the
// CCarInGarage constructor): on a car owned in the garage those write its stats
// into the saved garage row, and re-running the constructor corrupts it.
static void *volatile current_car_object;  // last car pointer seen in a vehicle update

// Called after every slider step, so it only notes the request; the vehicle
// update re-prepares once the slider has been still for REPREPARE_DELAY_MS.
enum { REPREPARE_DELAY_MS = 1000 };
static volatile uint32_t pending_car;
static volatile ULONGLONG pending_since;

static void rebuild_car_setup(uint32_t car_id) {
    if (!current_car_object || current_car != car_id) {
        log_line("LIVETUNE: tuned car isn't the one being driven; it applies the next time it's selected.\r\n");
        return;
    }
    pending_car = car_id;
    pending_since = GetTickCount64();
}

static void reprepare_when_settled(void) {
    if (!pending_car || GetTickCount64() - pending_since < REPREPARE_DELAY_MS) return;
    uint32_t car_id = pending_car;
    pending_car = 0;
    if (current_car != car_id) return;
    car_reprepare(car_id, current_car_object, log_line);  // the garage's copy (stats screens, next selection)
    car_rebuild(car_id, current_car_object, log_line);    // the live car: a same-car reload reuses it as is
}

static uint32_t car_model_id(void *car) {
    uint32_t id = 0;
    read_bytes((unsigned char *)car + 0x118, &id, sizeof id);
    return id;
}

__declspec(dllexport) BOOL WINAPI FH2PluginDatabaseUpdate(const FH2PluginDatabaseFrame *frame) {
    tunes_database(frame);
    return TRUE;  // a frame we can't use changes nothing, so it's not a failure
}

__declspec(dllexport) BOOL WINAPI FH2PluginInitialize(const FH2PluginContext *ctx) {
    if (!ctx || ctx->size < sizeof(FH2PluginContext) || ctx->api_version < 1) return FALSE;
    context = ctx;
    // LiveTune.ini beside the plugins folder: [Tab] Position=first|last (default first).
    wchar_t ini[MAX_PATH], position[16];
    swprintf(ini, MAX_PATH, L"%ls\\..\\LiveTune.ini", ctx->plugins_directory);
    GetPrivateProfileStringW(L"Tab", L"Position", L"first", position, ARRAYSIZE(position), ini);
    int first = _wcsicmp(position, L"last") != 0;
    tunes_init(log_line, rebuild_car_setup, ctx->plugins_directory);  // saved tunes, applied when the database arrives
    // The page's sliders bind to the properties, so register them before the page can load.
    if (tuning_tab_install(ctx->game_image, ctx->game_image_size, first, log_line) &&
        tuning_slider_install(ctx->game_image, ctx->game_image_size, log_line, tune_sliders, tune_slider_count,
                              tunes_get, tunes_set) &&
        tuning_page_install(ctx->game_image, ctx->game_image_size, log_line)) {
        // Leaving Tuning reuses Extra Options' Get in Car reload, so it must be on.
        // Its INI sits beside Azimuth.exe: mods/<this mod>/plugins/../../../
        wchar_t eo_ini[MAX_PATH];
        swprintf(eo_ini, MAX_PATH, L"%ls\\..\\..\\..\\FH2_ExtraOptions.ini", ctx->plugins_directory);
        int get_in_car = GetPrivateProfileIntW(L"Global", L"Enabled", 1, eo_ini) &&
                         GetPrivateProfileIntW(L"Frontend", L"GetInCarFromPauseMenu", 0, eo_ini);
        tuning_reload_install(ctx->game_image, ctx->game_image_size, log_line, get_in_car);
    }
    car_reprepare_init(ctx->game_image);
    car_rebuild_init(ctx->game_image);
    hp_display_init(ctx->game_image, ctx->game_image_size, log_line, ctx->plugins_directory);  // stat screens: display only
    log_line("LIVETUNE: Enhanced Tuning " ENHANCED_TUNING_VERSION " ready.\r\n");
    return TRUE;
}

__declspec(dllexport) void WINAPI FH2PluginVehicleUpdate(const FH2PluginVehicleFrame *frame) {
    if (!frame || frame->size < sizeof(FH2PluginVehicleFrame) || !frame->car) return;
    current_car = car_model_id(frame->car);
    current_car_object = frame->car;
    tunes_apply(current_car);
    reprepare_when_settled();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
