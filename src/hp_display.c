// Display-only horsepower/torque on the car-stat screens.
//
// Every stat panel (A_CarSelect "Power"/"Torque" bars, dealers, upgrades,
// tuning load/save) binds SimPeakPower / SimPeakTorque of one UI view model,
// the "car stats" item (vtable game+0x2dd1db8). Its two fill functions copy
// the numbers into the view model and nothing else:
//   game+0xccafe0  fill(vm, extra): by Data_Car Id ([vm+0xa8]); runs a
//                  temporary stats sim (0xaf11e0) and copies its getters
//   game+0xccb110  fill(vm, TRef<CCarInGarage>* car): copies the garage car's
//                  stats (CCarInGarage+0x278 power, +0x27c torque); releases the ref
// vm+0x98 = SimPeakPower, vm+0x9c = SimPeakTorque (floats, Data_Car units:
// power in 100 W, torque in 100 N*m; the XAML converter turns them into hp/lb-ft).
//
// We hook both: run the original, then multiply those two view-model floats by
// the car's Horsepower multiplier. The CCarInGarage, its garage row, PI and
// class are never touched, so nothing new reaches the save. docs/hp_display.md.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "entry_hook.h"
#include "hp_display.h"
#include "tunes.h"

enum {
    FILL_BY_ID = 0xccafe0,
    FILL_FROM_GARAGE = 0xccb110,
    CCAR_IN_GARAGE_VTABLE = 0x2c5f818,
    VM_CAR_ID = 0xa8,        // Data_Car Id the by-id fill computes
    VM_POWER = 0x98,         // SimPeakPower
    VM_TORQUE = 0x9c,        // SimPeakTorque
    GARAGE_CAR_ID = 0x40,    // CCarInGarage: Data_Car Id
    HORSEPOWER = 0,          // tune_sliders index
    MAX_LOGGED = 16,         // one line per car, the first time it's scaled
};

// push rbx; sub rsp, 0x90
static const unsigned char by_id_prologue[9] = {0x40, 0x53, 0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00};
// mov [rsp+0x10], rdx; push r14; sub rsp, 0x30
static const unsigned char from_garage_prologue[11] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x41, 0x56,
                                                       0x48, 0x83, 0xec, 0x30};

typedef void (*FillByIdFn)(void *vm, void *extra);
typedef void (*FillFromGarageFn)(void *vm, void **car);

static unsigned char *game;
static TabLog log_message;
static FillByIdFn fill_by_id;
static FillFromGarageFn fill_from_garage;
static volatile LONG logged_count;
static volatile uint32_t logged_cars[MAX_LOGGED];

static void log_first(uint32_t car, float multiplier, float power, float torque) {
    for (unsigned i = 0; i < MAX_LOGGED; ++i)
        if (logged_cars[i] == car) return;
    LONG slot = InterlockedIncrement(&logged_count) - 1;
    if (slot >= MAX_LOGGED) return;
    logged_cars[slot] = car;
    char line[220];
    // Data_Car units: power x100 W (/7.457 = hp), torque x100 N*m.
    snprintf(line, sizeof line,
             "LIVETUNE HP: car %u stat screen x%.2f: power %.1f -> %.1f (%.0f -> %.0f hp), torque %.3f -> %.3f.\r\n",
             car, multiplier, power, power * multiplier, power / 7.457f, power * multiplier / 7.457f, torque,
             torque * multiplier);
    log_message(line);
}

static void scale(unsigned char *vm, uint32_t car) {
    if (!vm || !car) return;
    float multiplier = tunes_multiplier_for(car, HORSEPOWER);
    if (multiplier == 1.0f || !(multiplier > 0.0f && multiplier <= 1000.0f)) return;
    volatile float *power = (volatile float *)(vm + VM_POWER);
    volatile float *torque = (volatile float *)(vm + VM_TORQUE);
    float p = *power, t = *torque;
    *power = p * multiplier;
    *torque = t * multiplier;
    log_first(car, multiplier, p, t);
}

static void hooked_fill_by_id(void *vm, void *extra) {
    fill_by_id(vm, extra);
    scale(vm, *(volatile uint32_t *)((unsigned char *)vm + VM_CAR_ID));
}

static void hooked_fill_from_garage(void *vm, void **car_ref) {
    // The original releases the reference, so read the car's id first.
    uint32_t car = 0;
    unsigned char *garage_car = car_ref ? (unsigned char *)*car_ref : NULL;
    if (garage_car && *(void **)garage_car == game + CCAR_IN_GARAGE_VTABLE)
        car = *(volatile uint32_t *)(garage_car + GARAGE_CAR_ID);
    fill_from_garage(vm, car_ref);
    scale(vm, car);
}

int hp_display_init(unsigned char *image, size_t image_size, TabLog log, const wchar_t *plugins_directory) {
    game = image;
    log_message = log;
    wchar_t ini[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%ls\\..\\LiveTune.ini", plugins_directory);
    if (!GetPrivateProfileIntW(L"Display", L"ScalePower", 1, ini)) {
        log("LIVETUNE HP: stat-screen power scaling off (LiveTune.ini [Display] ScalePower=0).\r\n");
        return 0;
    }
    // Both or neither, so every stat screen agrees. Check both before hooking either.
    if (FILL_FROM_GARAGE + sizeof from_garage_prologue >= image_size ||
        CCAR_IN_GARAGE_VTABLE + 8 >= image_size ||
        memcmp(image + FILL_BY_ID, by_id_prologue, sizeof by_id_prologue) ||
        memcmp(image + FILL_FROM_GARAGE, from_garage_prologue, sizeof from_garage_prologue)) {
        log("LIVETUNE HP: stat-screen code didn't match; power shown unscaled.\r\n");
        return 0;
    }
    fill_by_id = (FillByIdFn)entry_hook_install(image, image_size, FILL_BY_ID, by_id_prologue,
                                                sizeof by_id_prologue, (void *)hooked_fill_by_id);
    if (!fill_by_id) {
        log("LIVETUNE HP: couldn't hook the stat screen; power shown unscaled.\r\n");
        return 0;
    }
    fill_from_garage = (FillFromGarageFn)entry_hook_install(image, image_size, FILL_FROM_GARAGE, from_garage_prologue,
                                                            sizeof from_garage_prologue, (void *)hooked_fill_from_garage);
    log(fill_from_garage ? "LIVETUNE HP: stat screens show power and torque x the Horsepower slider (display only).\r\n"
                         : "LIVETUNE HP: garage-car stat hook failed; only dealer/by-id stat screens are scaled.\r\n");
    return 1;
}
