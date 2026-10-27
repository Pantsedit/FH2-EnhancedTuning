// Enhanced Tuning: per-car multipliers written into the game database.
//
// The spec block inside the car is filled from the game database when the car
// is built, and the physics is built from that. So a tune edits the car's rows
// in Extra Options' merged in-memory database; the next rebuild (leaving
// Tuning, or Get in Car) drives with it. Nothing is written to disk except our
// own tunes.ini.
//
// Every slider is a multiplier on stock values. Stock is read once per row
// (before LiveTune first changes it) and kept, so moving a slider back and
// forth never compounds, and 1.0 writes the stock value back.
//
// MW Handling is the exception: an intensity t from 0 to 1 that blends this
// car's rows from stock toward an "NFS Most Wanted-like" target
// (docs/mw_handling.md 3.2, Tier 1: per-car rows only). Each column gets
//     blended = stock + (target - stock) * t
// from the remembered stock value. Where another slider edits the same column
// (Grip, Brake force, Downforce), both sliders write the same value:
//     final = blended * that slider's multiplier
// So the order of edits doesn't matter, and both back at stock (t = 0, x1.0)
// writes the exact stock value. With t = 0 every other slider behaves exactly
// as before.
//
// Most edits scale a column (value = stock * multiplier). A few transform it
// instead: Top Speed divides the final drive ratio (a longer ratio is a higher
// top speed), Boost scales only the part of a turbo/supercharger torque scale
// above 1.0 (the boost itself), and Low-end/Top-end torque reshape the stock
// engine's torque curve cells (v0..vN), each cell by a blend of the two
// sliders that fades from Low-end below ~35% of the redline to Top-end above
// ~65%. Horsepower scales the curve's TorqueScale column, so the three never
// write the same value and compose as a product. Curve cells are read and
// written bit-exactly (see curve_for), so both back at x1.0 restores them.
// Every write spells the double exactly (sql_double), never as rounded text.
//
// Extra Options only promises the connection during FH2PluginDatabaseUpdate.
// Keeping it is outside that contract, so it's only used on the thread that
// callback ran on, and a new merge (only at startup) replaces it.

#include <windows.h>
#include <stdarg.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "fh2_plugin.h"
#include "tunes.h"

enum {
    SQLITE_ROW = 100, SQLITE_DONE = 101,
    MAX_TUNES = 64,     // cars with a tune this session
    MAX_EDITS = 12,     // columns one slider changes
    MAX_ROWS = 8,       // rows one column edit may touch (upgrade levels)
    MAX_STOCK = 8192,   // remembered stock values
    MAX_CURVES = 256,   // remembered stock torque curves (rows of List_TorqueCurve)
    CURVE_CELLS = 246,  // List_TorqueCurve v0..v245
};
// Slider indices, same order as tune_sliders.
enum { S_HORSEPOWER, S_LOW_END, S_TOP_END, S_BOOST, S_REDLINE, S_TOP_SPEED, S_WEIGHT, S_STEERING, S_BRAKES,
       S_GRIP_FRONT, S_GRIP_REAR, S_DRAG, S_DOWNFORCE_FRONT, S_DOWNFORCE_REAR, S_MW, SLIDERS };
#define FIXED 1e9  // stock values are read as integers in units of 1e-9 (the frame only reads int64)

// Help text says where each change lands; shared rows are called out.
const SliderSpec tune_sliders[] = {
    {"LiveTuneHorsepower", "ENGINE", "Horsepower",
     "Scales engine torque at every rpm, so horsepower changes by the same factor. Low-end and Top-end torque "
     "multiply on top. The marker shows stock. Applies when the car is rebuilt: leave Tuning, or Get in Car.",
     0.5f, 20.0f, 0.05f, 0.1f, 1.0f, 1.0f},
    {"LiveTuneLowEndTorque", "ENGINE", "Low-end torque",
     "Scales torque low in the rev range (full effect up to about a third of the redline, fading out by two "
     "thirds), for pull out of corners and off the line. Multiplies with Horsepower and Top-end torque. "
     "Changes the stock engine's torque curves (rarely shared; the log names any other car using that engine).",
     0.5f, 2.0f, 0.05f, 0.05f, 0.25f, 1.0f},
    {"LiveTuneTopEndTorque", "ENGINE", "Top-end torque",
     "Scales torque high in the rev range (from about two thirds of the redline up), for top-end pull and peak "
     "horsepower. Multiplies with Horsepower and Low-end torque. Changes the stock engine's torque curves "
     "(rarely shared; the log names any other car using that engine).",
     0.5f, 2.0f, 0.05f, 0.05f, 0.25f, 1.0f},
    {"LiveTuneBoost", "ENGINE", "Boost",
     "Scales turbo or supercharger boost: x2 doubles the torque the boost adds, x0 removes it. Covers every "
     "turbo and supercharger level for the stock engine. Naturally aspirated cars only change once a turbo or "
     "supercharger is fitted. Shared by every car with the same stock engine.",
     0.0f, 3.0f, 0.05f, 0.05f, 0.25f, 1.0f},
    {"LiveTuneRedline", "ENGINE", "Rev limit",
     "Scales the redline (and the torque curve's top end when needed). Shared by every car with the same stock engine.",
     0.5f, 2.0f, 0.01f, 0.05f, 0.25f, 1.0f},
    {"LiveTuneTopSpeed", "DRIVETRAIN", "Top speed",
     "Lengthens (higher) or shortens (lower) the final drive of every transmission level of the stock drivetrain. "
     "Longer gearing raises top speed if the engine can pull it, but accelerates slower. A race transmission's own "
     "Final Drive setting may override it. Rarely shared; the log names any other car using that drivetrain.",
     0.7f, 1.5f, 0.01f, 0.01f, 0.05f, 1.0f},
    {"LiveTuneWeight", "CHASSIS", "Weight",
     "Scales the car's mass for every weight-reduction level of this car. Lighter accelerates, brakes and turns "
     "quicker; heavier is more planted.",
     0.5f, 1.5f, 0.01f, 0.01f, 0.05f, 1.0f},
    {"LiveTuneSteering", "CHASSIS", "Steering lock",
     "Scales the maximum steering angle (wheel and controller) of this car. Higher turns tighter at low speed.",
     0.7f, 1.3f, 0.01f, 0.01f, 0.05f, 1.0f},
    {"LiveTuneBrakes", "BRAKES", "Brake force",
     "Scales braking friction for every brake upgrade level of this car.",
     0.25f, 4.0f, 0.05f, 0.05f, 0.25f, 1.0f},
    {"LiveTuneGripFront", "GRIP", "Front",
     "Scales front tire friction, cornering and braking, for every chassis upgrade level of this car.",
     0.25f, 4.0f, 0.05f, 0.05f, 0.25f, 1.0f},
    {"LiveTuneGripRear", "GRIP", "Rear",
     "Scales rear tire friction, cornering and traction, for every chassis upgrade level of this car.",
     0.25f, 4.0f, 0.05f, 0.05f, 0.25f, 1.0f},
    {"LiveTuneDrag", "AERO", "Drag",
     "Scales aerodynamic drag. Lower is faster on the straights.",
     0.25f, 4.0f, 0.05f, 0.05f, 0.25f, 1.0f},
    {"LiveTuneDownforceFront", "AERO", "Front downforce",
     "Scales the body's front downforce. Cars with no stock downforce are unaffected.",
     0.25f, 4.0f, 0.05f, 0.05f, 0.25f, 1.0f},
    {"LiveTuneDownforceRear", "AERO", "Rear downforce",
     "Scales the body's rear downforce. Cars with no stock downforce are unaffected.",
     0.25f, 4.0f, 0.05f, 0.05f, 0.25f, 1.0f},
    {"LiveTuneMwHandling", "MW HANDLING", "MW Handling",
     "Blends this car toward NFS Most Wanted-style handling: 0 is stock, 1 is the full preset. "
     "More grip (slower classes gain more, up to 1.8x, rear a bit more than front), a downforce floor that also "
     "cures rear lift, a stronger rear stability boost, less steering lock at speed, quicker steering and "
     "countersteer, and a stronger handbrake and brakes. Only this car's rows change. The Grip, Brake force and "
     "Downforce sliders multiply on top. Applies when the car is rebuilt: leave Tuning, or Get in Car.",
     0.0f, 1.0f, 0.05f, 0.05f, 0.25f, 0.0f},
};
const unsigned tune_slider_count = ARRAYSIZE(tune_sliders);
_Static_assert(ARRAYSIZE(tune_sliders) == SLIDERS, "slider indices");

// Stock setting of a slider: 1.0 (a multiplier), MW Handling 0.0 (an intensity).
static float stock_setting(unsigned slider) {
    return slider < tune_slider_count ? tune_sliders[slider].stock : 1.0f;
}

// MW Handling targets (docs/mw_handling.md 3.1-3.2, safe ranges from 3.5).
// Each is the value at full intensity, computed from this row's stock value.
enum {
    MW_NONE,
    MW_GRIP_FRONT,      // stock * gripMul, gripMul = clamp(1.5 / SimLatGees60MPH, 1.0, 1.8)
    MW_GRIP_REAR,       // stock * min(gripMul * 1.05, 1.8): a stable, rear-biased balance
    MW_DOWNFORCE_FRONT, // max(stock, min(max(2 * stock, 94), 600))
    MW_DOWNFORCE_REAR,  // max(stock, min(max(2 * stock, 173, 1.6 * front target), 600))
    MW_REAR_STABILITY,  // FixListingRearFricScale: max(stock, 1.5)
    MW_STEER_LOCK,      // SteerSpeedSensitiveMinMaxAngle: min(stock, 3.25)
    MW_STEER_RATE,      // SteerMaxAngVelTurning: max(stock, 150)
    MW_COUNTERSTEER,    // SteerAngVelCountersteer: max(stock, 1000)
    MW_HANDBRAKE,       // BiasHandbrake: max(stock, min(2.5 * stock, 5))
    MW_BRAKING,         // GameFrictionScaleBraking: stock * 1.3
    MW_RULES
};
#define MW_GRIP_GEES 1.5      // target lateral g at 60 mph (MW's 3.8 g is too much for FH2's AI and tracks)
#define MW_GRIP_MAX 1.8       // friction scales stay <= 1.8x stock
#define MW_REAR_BIAS 1.05
#define MW_DOWNFORCE_FLOOR_FRONT 94.0   // S1-class medians of Data_Car downforce (4.2); also cures rear lift
#define MW_DOWNFORCE_FLOOR_REAR 173.0
#define MW_DOWNFORCE_REAR_RATIO 1.6     // rear >= 1.6x front
#define MW_DOWNFORCE_MAX 600.0          // about the DB maximum (575)
// Extra Data_Car column a target reads for this car; checked with the edit.
static const char *const mw_needs[MW_RULES] = {
    [MW_GRIP_FRONT] = "SimLatGees60MPH",
    [MW_GRIP_REAR] = "SimLatGees60MPH",
    [MW_DOWNFORCE_REAR] = "BodyAeroForwardDownforceFront",
};

// How an edit turns the stock value and the slider into the value written.
enum {
    EDIT_SCALE,    // stock * multiplier (after the MW Handling blend, if any)
    EDIT_INVERSE,  // stock / multiplier: a higher setting is a lower ratio
    EDIT_BOOST,    // 1 + (stock - 1) * multiplier above 1.0; at or below 1.0 kept (vacuum/lag)
    EDIT_CURVE,    // the torque curve cells v0..vN (apply_curve); column is only checked
};

typedef struct {
    const char *table, *column;
    const char *rows;      // WHERE clause picking this car's rows; every %u is the Data_Car Id
    const char *at_least;  // instead of scaling: keep the column >= this (already written) column of the row
    const char *sharers;   // optional: how many cars use the same rows; every %u is the Data_Car Id
    int mw;                // MW Handling target of this column (MW_NONE: not blended)
    int with;              // MW Handling edits only: 1 + the slider whose multiplier also scales the column
    int kind;              // EDIT_*
    int optional;          // no rows for this car is normal (not a failure)
} Edit;

#define STOCK_ENGINE "EngineID IN (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal=%u AND IsStock=1)"
#define CAR_BODY "CarbodyId IN (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal=%u)"
#define ENGINE_SHARERS "SELECT COUNT(DISTINCT Ordinal) FROM List_UpgradeEngine WHERE IsStock=1 AND " STOCK_ENGINE
#define STOCK_DRIVETRAIN "DrivetrainID IN (SELECT DrivetrainID FROM List_UpgradeDrivetrain WHERE Ordinal=%u AND IsStock=1)"
#define STOCK_CURVES "TorqueCurveID IN (SELECT TorqueCurveFullThrottleID FROM List_UpgradeEngineCamshaft WHERE " STOCK_ENGINE ")"

// Same order as tune_sliders. Verified against gamedbRC.slt (FH2 1.0.0.64); the
// physics builder's column reads are at game+0x105b6f6 (Mass, InitialMass),
// +0x10622fe/+0x1062641 (SteerMaxAngle, SteerMaxAngleFiltered), +0x10646a3
// (FinalDriveRatio), +0x1064a2e (turbo MaxScale) and +0x10627fd/+0x1062852
// (supercharger ZeroRPMScale, RedlineRPMScale).
static const Edit edits[][MAX_EDITS] = {
    // The stock engine's torque curves (each car has its own): the builder clamps
    // Data_Car.GameTorqueScale to [0.5, 1.5], the curves' TorqueScale isn't
    // clamped. SimPeakPower/SimPeakTorque are the car's quoted figures (what the
    // stat screens show), scaled to match.
    {{"List_TorqueCurve", "TorqueScale", STOCK_CURVES, NULL, ENGINE_SHARERS},
     {"Data_Car", "SimPeakPower", "Id=%u"},
     {"Data_Car", "SimPeakTorque", "Id=%u"}},
    // Low-end and Top-end write the same curve cells (each from both sliders).
    {{"List_TorqueCurve", "v0", STOCK_CURVES, NULL, ENGINE_SHARERS, .kind = EDIT_CURVE}},
    {{"List_TorqueCurve", "v0", STOCK_CURVES, NULL, ENGINE_SHARERS, .kind = EDIT_CURVE}},
    // Boost: turbo MaxScale is the full-boost torque scale, 1 + StockBoost-bar / 2
    // (PowerMaxScale/PowerMinScale are where boost builds, MinScale the lag below
    // 1.0). Superchargers scale torque from ZeroRPMScale at 0 rpm to
    // RedlineRPMScale at the redline. Rows are per engine, every upgrade level;
    // only one table has rows for a given car, the Quad table has none at all.
    {{"List_UpgradeEngineTurboSingle", "MaxScale", STOCK_ENGINE, NULL, ENGINE_SHARERS, .kind = EDIT_BOOST, .optional = 1},
     {"List_UpgradeEngineTurboTwin", "MaxScale", STOCK_ENGINE, NULL, ENGINE_SHARERS, .kind = EDIT_BOOST, .optional = 1},
     {"List_UpgradeEngineTurboQuad", "MaxScale", STOCK_ENGINE, NULL, ENGINE_SHARERS, .kind = EDIT_BOOST, .optional = 1},
     {"List_UpgradeEngineCSC", "ZeroRPMScale", STOCK_ENGINE, NULL, ENGINE_SHARERS, .kind = EDIT_BOOST, .optional = 1},
     {"List_UpgradeEngineCSC", "RedlineRPMScale", STOCK_ENGINE, NULL, ENGINE_SHARERS, .kind = EDIT_BOOST, .optional = 1},
     {"List_UpgradeEngineDSC", "ZeroRPMScale", STOCK_ENGINE, NULL, ENGINE_SHARERS, .kind = EDIT_BOOST, .optional = 1},
     {"List_UpgradeEngineDSC", "RedlineRPMScale", STOCK_ENGINE, NULL, ENGINE_SHARERS, .kind = EDIT_BOOST, .optional = 1}},
    {{"List_UpgradeEngineCamshaft", "RedlineRPM", STOCK_ENGINE, NULL, ENGINE_SHARERS},
     {"List_UpgradeEngineCamshaft", "TorqueCurveMaxRPM", STOCK_ENGINE, "RedlineRPM"},
     {"Data_Car", "SimRedlineAngVel", "Id=%u"}},
    // Every transmission level of the stock drivetrain (one per car, except
    // drivetrain 380, which cars 631 and 2500 share).
    {{"List_UpgradeDrivetrainTransmission", "FinalDriveRatio", STOCK_DRIVETRAIN, NULL,
      "SELECT COUNT(DISTINCT Ordinal) FROM List_UpgradeDrivetrain WHERE IsStock=1 AND " STOCK_DRIVETRAIN,
      .kind = EDIT_INVERSE}},
    // The body's mass per weight level (CarBodyId is per car). InitialMass is the
    // stock-level mass the builder reads next to it; both scale so they stay in step.
    {{"List_UpgradeCarBodyWeight", "Mass", CAR_BODY},
     {"List_UpgradeCarBodyWeight", "InitialMass", CAR_BODY}},
    // SteerMaxAngle and SteerMaxAngleFiltered (the smaller, controller lock).
    {{"Data_Car", "SteerMaxAngle", "Id=%u"},
     {"Data_Car", "SteerMaxAngleFiltered", "Id=%u"}},
    {{"List_UpgradeBrakes", "GameFrictionScaleBraking", "Ordinal=%u", .mw = MW_BRAKING}},
    {{"List_UpgradeCarBodyChassisStiffness", "FrontLatFrictionScale", CAR_BODY, .mw = MW_GRIP_FRONT},
     {"List_UpgradeCarBodyChassisStiffness", "FrontLongFrictionScale", CAR_BODY, .mw = MW_GRIP_FRONT}},
    {{"List_UpgradeCarBodyChassisStiffness", "RearLatFrictionScale", CAR_BODY, .mw = MW_GRIP_REAR},
     {"List_UpgradeCarBodyChassisStiffness", "RearLongFrictionScale", CAR_BODY, .mw = MW_GRIP_REAR}},
    {{"Data_Car", "GameDragScale", "Id=%u"}},
    {{"Data_Car", "BodyAeroForwardDownforceFront", "Id=%u", .mw = MW_DOWNFORCE_FRONT}},
    {{"Data_Car", "BodyAeroForwardDownforceRear", "Id=%u", .mw = MW_DOWNFORCE_REAR}},
    // MW Handling. Per-car rows only: no shared tire, compound or engine rows.
    {{"List_UpgradeCarBodyChassisStiffness", "FrontLatFrictionScale", CAR_BODY, .mw = MW_GRIP_FRONT, .with = 1 + S_GRIP_FRONT},
     {"List_UpgradeCarBodyChassisStiffness", "FrontLongFrictionScale", CAR_BODY, .mw = MW_GRIP_FRONT, .with = 1 + S_GRIP_FRONT},
     {"List_UpgradeCarBodyChassisStiffness", "RearLatFrictionScale", CAR_BODY, .mw = MW_GRIP_REAR, .with = 1 + S_GRIP_REAR},
     {"List_UpgradeCarBodyChassisStiffness", "RearLongFrictionScale", CAR_BODY, .mw = MW_GRIP_REAR, .with = 1 + S_GRIP_REAR},
     {"Data_Car", "BodyAeroForwardDownforceFront", "Id=%u", .mw = MW_DOWNFORCE_FRONT, .with = 1 + S_DOWNFORCE_FRONT},
     {"Data_Car", "BodyAeroForwardDownforceRear", "Id=%u", .mw = MW_DOWNFORCE_REAR, .with = 1 + S_DOWNFORCE_REAR},
     {"Data_Car", "FixListingRearFricScale", "Id=%u", .mw = MW_REAR_STABILITY},
     {"Data_Car", "SteerSpeedSensitiveMinMaxAngle", "Id=%u", .mw = MW_STEER_LOCK},
     {"Data_Car", "SteerMaxAngVelTurning", "Id=%u", .mw = MW_STEER_RATE},
     {"Data_Car", "SteerAngVelCountersteer", "Id=%u", .mw = MW_COUNTERSTEER},
     {"List_UpgradeBrakes", "BiasHandbrake", "Ordinal=%u", .mw = MW_HANDBRAKE},
     {"List_UpgradeBrakes", "GameFrictionScaleBraking", "Ordinal=%u", .mw = MW_BRAKING, .with = 1 + S_BRAKES}},
};
_Static_assert(ARRAYSIZE(edits) == ARRAYSIZE(tune_sliders), "one edit list per slider");
_Static_assert(ARRAYSIZE(tune_sliders) <= MAX_SLIDERS, "slider count");

typedef struct {
    uint32_t car;                  // Data_Car Id
    float multiplier[MAX_SLIDERS]; // what the sliders show
    volatile LONG dirty[MAX_SLIDERS];
    volatile LONG save;            // changed by the player: write tunes.ini after applying
} CarTune;

typedef struct {
    const char *table, *column;  // per column, not per Edit: MW Handling and another slider share it
    int64_t row;
    double value;
} Stock;

static TabLog log_line;
static TuneRebuild rebuild_car;
static wchar_t ini_path[MAX_PATH];

static FH2PluginDatabaseFrame database;
static int have_database;
static DWORD database_thread;
static int column_ok[ARRAYSIZE(edits)][MAX_EDITS];  // checked on each new connection
static int slider_ok[ARRAYSIZE(edits)];
static int mw_ok[MW_RULES];  // MW Handling can blend this target (its columns exist)

// Stock cells of one torque curve row, read bit-exactly; a cell that couldn't be
// (no value, out of range, or not exactly representable) is never written.
typedef struct {
    int64_t row;
    unsigned count;  // NumTorqueValues
    double value[CURVE_CELLS];
    unsigned char exact[CURVE_CELLS];
} CurveStock;

static CarTune tunes[MAX_TUNES];
static Stock stock[MAX_STOCK];
static unsigned stock_count;
static CurveStock curves[MAX_CURVES];
static unsigned curve_count;
static int shared_noted;  // the "rows are shared" note was logged for this slider change
static volatile uint32_t driven_car;  // last car seen in a vehicle update
static volatile LONG tunes_dirty;

static void logf_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void logf_line(const char *format, ...) {
    char line[320];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    log_line(line);
}

static int db_exec(const char *sql, int64_t *first_column) {
    void *statement = NULL;
    const char *tail = NULL;
    if (database.prepare(database.database, sql, -1, &statement, &tail) != 0 || !statement) return 0;
    int result = database.step(statement);
    if (result == SQLITE_ROW && first_column) *first_column = database.column_int64(statement, 0);
    database.finalize(statement);
    return result == SQLITE_ROW || result == SQLITE_DONE;
}

// Formats an edit's SQL; the car fills every %u (at most three).
static void edit_sql(char *sql, size_t size, const char *prefix, const char *clause, uint32_t car) {
    char where[300];
    snprintf(where, sizeof where, clause, car, car, car);
    snprintf(sql, size, "%s%s", prefix, where);
}

static CarTune *tune_for(uint32_t car, int create) {
    for (unsigned i = 0; i < MAX_TUNES; ++i)
        if (tunes[i].car == car) return &tunes[i];
    if (!create) return NULL;
    for (unsigned i = 0; i < MAX_TUNES; ++i)
        if (!tunes[i].car) {
            CarTune *t = &tunes[i];
            memset(t, 0, sizeof *t);
            for (unsigned s = 0; s < MAX_SLIDERS; ++s) t->multiplier[s] = stock_setting(s);
            t->car = car;
            return t;
        }
    return NULL;
}

// The columns that read a value exactly, after the row's other columns:
//   CAST(ROUND(v*1e9)), typeof numeric, then v*2^k and "is that exact" for
//   k = 60, 47, 40, 31 (|v| < 8, 65536, 2^23, 2^32).
// read_value takes the first exact power of two, else the 1e9 integer (exact
// for values with at most 9 decimals). The frame can only read int64.
static const int exact_shifts[] = {60, 47, 40, 31};
static int value_columns(char *out, size_t size, const char *column) {
    int n = snprintf(out, size, "CAST(ROUND(%s*%.0f) AS INTEGER), typeof(%s) IN ('real','integer')", column, FIXED,
                     column);
    for (unsigned i = 0; i < ARRAYSIZE(exact_shifts) && n > 0 && (size_t)n < size; ++i) {
        double scale = ldexp(1.0, exact_shifts[i]);
        n += snprintf(out + n, size - (size_t)n, ", CAST(%s*%.0f.0 AS INTEGER), %s=CAST(%s*%.0f.0 AS INTEGER)/%.0f.0", column,
                      scale, column, column, scale, scale);
    }
    return n > 0 && (size_t)n < size;
}
static double read_value(void *statement, int first) {
    for (unsigned i = 0; i < ARRAYSIZE(exact_shifts); ++i)
        if (database.column_int64(statement, first + 3 + 2 * (int)i))
            return ldexp((double)database.column_int64(statement, first + 2 + 2 * (int)i), -exact_shifts[i]);
    return database.column_int64(statement, first) / FIXED;
}

// Stock value of a row: remembered from the first time LiveTune touched it, so
// a row shared by several tuned cars is never read back already scaled.
static Stock *stock_for(const char *table, const char *column, int64_t row, double current) {
    for (unsigned i = 0; i < stock_count; ++i)
        if (stock[i].row == row && !strcmp(stock[i].column, column) && !strcmp(stock[i].table, table))
            return &stock[i];
    if (stock_count >= MAX_STOCK) return NULL;
    stock[stock_count] = (Stock){table, column, row, current};
    return &stock[stock_count++];
}

// Stock value of one of this car's Data_Car columns (read once, then remembered
// like any edited row, so a later edit or a game rewrite never shifts it).
static int car_stock(const char *column, uint32_t car, double *value) {
    char sql[2000], columns[1600];  // value_columns: about 5 names a rung
    void *statement = NULL;
    const char *tail = NULL;
    int ok = 0;
    if (!value_columns(columns, sizeof columns, column)) return 0;
    snprintf(sql, sizeof sql, "SELECT rowid, %s FROM Data_Car WHERE Id=%u", columns, car);
    if (database.prepare(database.database, sql, -1, &statement, &tail) != 0 || !statement) return 0;
    if (database.step(statement) == SQLITE_ROW && database.column_int64(statement, 2)) {
        Stock *s = stock_for("Data_Car", column, database.column_int64(statement, 0), read_value(statement, 1));
        if (s) { *value = s->value; ok = 1; }
    }
    database.finalize(statement);
    return ok;
}

static double clamp(double value, double low, double high) {
    return value < low ? low : value > high ? high : value;
}

// MW Handling value of a column at full intensity, from its stock value.
static int mw_target(int rule, uint32_t car, double stock_value, double *target) {
    double gees = 0, front = 0, grip = 1.0, front_target;
    switch (rule) {
    case MW_GRIP_FRONT:
    case MW_GRIP_REAR:
        // Scaled by class: slow cars gain most, the fastest nothing; keeps the order.
        if (!car_stock("SimLatGees60MPH", car, &gees)) return 0;
        if (gees > 0) grip = clamp(MW_GRIP_GEES / gees, 1.0, MW_GRIP_MAX);
        if (rule == MW_GRIP_REAR) grip = fmin(grip * MW_REAR_BIAS, MW_GRIP_MAX);
        *target = stock_value * grip;
        return 1;
    case MW_DOWNFORCE_FRONT:
        // A floor, not just a multiplier: some cars have lift (negative values).
        *target = fmax(stock_value, fmin(fmax(2 * stock_value, MW_DOWNFORCE_FLOOR_FRONT), MW_DOWNFORCE_MAX));
        return 1;
    case MW_DOWNFORCE_REAR:
        if (!car_stock("BodyAeroForwardDownforceFront", car, &front)) return 0;
        front_target = fmax(front, fmin(fmax(2 * front, MW_DOWNFORCE_FLOOR_FRONT), MW_DOWNFORCE_MAX));
        *target = fmax(stock_value, fmin(fmax(fmax(2 * stock_value, MW_DOWNFORCE_FLOOR_REAR),
                                              MW_DOWNFORCE_REAR_RATIO * front_target), MW_DOWNFORCE_MAX));
        return 1;
    case MW_REAR_STABILITY: *target = fmax(stock_value, 1.5); return 1;
    case MW_STEER_LOCK: *target = fmin(stock_value, 3.25); return 1;
    case MW_STEER_RATE: *target = fmax(stock_value, 150); return 1;
    case MW_COUNTERSTEER: *target = fmax(stock_value, 1000); return 1;
    case MW_HANDBRAKE: *target = fmax(stock_value, fmin(2.5 * stock_value, 5)); return 1;
    case MW_BRAKING: *target = stock_value * 1.3; return 1;
    default: return 0;
    }
}

// A double as SQL text that parses back to exactly the same bits:
// mantissa / 2^k, both integers (decimal "%.17g" relies on the parser rounding right).
static void sql_double(char *out, size_t size, double value) {
    int exponent = 0;
    if (value == 0 || !isfinite(value)) {
        snprintf(out, size, "%.17g", value);
        return;
    }
    double fraction = frexp(value, &exponent);  // value = fraction * 2^exponent, 0.5 <= |fraction| < 1
    long long mantissa = (long long)ldexp(fraction, 53);
    int shift = 53 - exponent;  // value = mantissa / 2^shift
    while (shift > 0 && !(mantissa & 1)) { mantissa /= 2; --shift; }
    if (shift <= 0 && shift >= -9)
        snprintf(out, size, "%lld.0", mantissa * (1LL << -shift));
    else if (shift > 0 && shift <= 62)
        snprintf(out, size, "(%lld.0/%lld.0)", mantissa, 1LL << shift);
    else
        snprintf(out, size, "%.17g", value);
}

static double rounded(float setting) {
    return floor(setting * 10000.0 + 0.5) / 10000.0;  // 0.9f is 0.8999999762
}

static int apply_edit(const Edit *edit, const CarTune *t, unsigned index) {
    char sql[2400], prefix[1800], columns[1600];
    uint32_t car = t->car;
    const char *slider = tune_sliders[index].property;
    // final = (stock blended toward the MW target by t) * the column's own slider multiplier
    unsigned scale_slider = edit->with ? (unsigned)edit->with - 1 : index;
    double multiplier = scale_slider == S_MW || !slider_ok[scale_slider] ? 1.0 : rounded(t->multiplier[scale_slider]);
    double mw = edit->mw && slider_ok[S_MW] && mw_ok[edit->mw] ? rounded(t->multiplier[S_MW]) : 0;
    int64_t rows[MAX_ROWS];
    double current[MAX_ROWS];
    unsigned count = 0, too_many = 0, not_numeric = 0;

    if (!value_columns(columns, sizeof columns, edit->column)) {
        logf_line("LIVETUNE: car %u %s: %s.%s: column name too long; not changed.\r\n", car, slider, edit->table,
                  edit->column);
        return 0;
    }
    snprintf(prefix, sizeof prefix, "SELECT rowid, %s FROM %s WHERE ", columns, edit->table);
    edit_sql(sql, sizeof sql, prefix, edit->rows, car);
    void *statement = NULL;
    const char *tail = NULL;
    if (database.prepare(database.database, sql, -1, &statement, &tail) != 0 || !statement) {
        logf_line("LIVETUNE: car %u %s: couldn't read %s.%s.\r\n", car, slider, edit->table, edit->column);
        return 0;
    }
    int step;
    while ((step = database.step(statement)) == SQLITE_ROW) {
        if (!database.column_int64(statement, 2)) { ++not_numeric; continue; }
        if (count == MAX_ROWS) { too_many = 1; break; }
        rows[count] = database.column_int64(statement, 0);
        current[count++] = read_value(statement, 1);
    }
    database.finalize(statement);
    if (too_many || (step != SQLITE_DONE && step != SQLITE_ROW)) {
        logf_line("LIVETUNE: car %u %s: %s.%s has more rows than expected; not changed.\r\n", car, slider,
                  edit->table, edit->column);
        return 0;
    }
    if (!count) {
        if (edit->optional && !not_numeric) return 1;  // e.g. no supercharger rows on a turbo engine
        logf_line("LIVETUNE: car %u %s: no %s rows for this car%s; not changed.\r\n", car, slider, edit->table,
                  not_numeric ? " with a value" : "");
        return 0;
    }

    // Writes only after the read is finished, one row at a time by rowid.
    unsigned written = 0;
    double first_stock = 0, first_value = 0;
    for (unsigned r = 0; r < count; ++r) {
        Stock *s = stock_for(edit->table, edit->column, rows[r], current[r]);
        if (!s) { log_line("LIVETUNE: too many stock values remembered; tune not changed.\r\n"); break; }
        double blended = s->value, target = 0;
        if (mw) {
            if (!mw_target(edit->mw, car, s->value, &target)) {
                logf_line("LIVETUNE: car %u %s: couldn't read this car's Data_Car values for %s.%s.\r\n", car, slider,
                          edit->table, edit->column);
                break;
            }
            blended = s->value + (target - s->value) * mw;
        }
        double value = blended * multiplier;
        if (multiplier == 1.0) value = blended;  // stock (or the MW blend) exactly
        else if (edit->kind == EDIT_INVERSE) value = blended / multiplier;
        else if (edit->kind == EDIT_BOOST) value = blended > 1.0 ? 1.0 + (blended - 1.0) * multiplier : blended;
        char number[64];
        int64_t changed = 0;
        sql_double(number, sizeof number, edit->at_least ? s->value : value);
        if (edit->at_least)
            snprintf(sql, sizeof sql, "UPDATE %s SET %s=MAX(%s, %s) WHERE rowid=%lld", edit->table, edit->column,
                     number, edit->at_least, (long long)rows[r]);
        else
            snprintf(sql, sizeof sql, "UPDATE %s SET %s=%s WHERE rowid=%lld", edit->table, edit->column, number,
                     (long long)rows[r]);
        if (!db_exec(sql, NULL) || !db_exec("SELECT changes()", &changed) || changed != 1) break;
        if (!written++) { first_stock = s->value; first_value = value; }
    }
    if (written != count) {
        logf_line("LIVETUNE: car %u %s: %s.%s update failed after %u of %u rows.\r\n", car, slider, edit->table,
                  edit->column, written, count);
        return 0;
    }
    if (edit->at_least)
        logf_line("LIVETUNE: car %u %s: %s.%s kept >= %s (%u rows).\r\n", car, slider, edit->table, edit->column,
                  edit->at_least, count);
    else if (edit->mw && (mw || index == S_MW))
        logf_line("LIVETUNE: car %u %s: %s.%s %.3f -> %.3f (MW %.2f x%.2f, %u row%s).\r\n", car, slider, edit->table,
                  edit->column, first_stock, first_value, mw, multiplier, count, count == 1 ? "" : "s");
    else
        logf_line("LIVETUNE: car %u %s x%.2f: %s.%s %.3f -> %.3f (%u row%s).\r\n", car, slider, multiplier,
                  edit->table, edit->column, first_stock, first_value, count, count == 1 ? "" : "s");
    if (edit->sharers && !shared_noted) {
        int64_t cars = 0;
        edit_sql(sql, sizeof sql, "", edit->sharers, car);
        if (db_exec(sql, &cars) && cars > 1) {
            logf_line("LIVETUNE: note: those %s rows are shared by %lld cars; all of them get this change.\r\n",
                      edit->table, (long long)cars);
            shared_noted = 1;
        }
    }
    return 1;
}

// Low-end/Top-end weight of curve cell i: 1 (all Low-end) up to CURVE_LOW of
// the stock redline, 0 (all Top-end) from CURVE_HIGH, smoothstep between.
#define CURVE_LOW 0.35
#define CURVE_HIGH 0.65
static double low_end_weight(unsigned cell, double redline_cell) {
    double x = redline_cell > 0 ? cell / redline_cell : 1.0;
    if (x <= CURVE_LOW) return 1.0;
    if (x >= CURVE_HIGH) return 0.0;
    double u = (x - CURVE_LOW) / (CURVE_HIGH - CURVE_LOW);
    return 1.0 - u * u * (3.0 - 2.0 * u);
}

// Stock cells of a curve row: remembered the first time, read bit-exactly as
// v * 2^60 (exact for 2^-8 <= |v| < 8; the curves are normalised, 0..3).
static CurveStock *curve_for(int64_t row) {
    static char sql[CURVE_CELLS * 200 + 200];  // about 155 bytes a cell
    for (unsigned i = 0; i < curve_count; ++i)
        if (curves[i].row == row) return &curves[i];
    if (curve_count >= MAX_CURVES) return NULL;
    CurveStock *c = &curves[curve_count];
    size_t n = (size_t)snprintf(sql, sizeof sql, "SELECT NumTorqueValues");
    for (unsigned i = 0; i < CURVE_CELLS && n < sizeof sql; ++i)
        n += (size_t)snprintf(sql + n, sizeof sql - n,
                              ", CAST(v%u*1152921504606846976.0 AS INTEGER), "
                              "typeof(v%u) IN ('real','integer') AND v%u=CAST(v%u*1152921504606846976.0 AS INTEGER)/1152921504606846976.0",
                              i, i, i, i);
    if (n >= sizeof sql) return NULL;
    snprintf(sql + n, sizeof sql - n, " FROM List_TorqueCurve WHERE rowid=%lld", (long long)row);
    void *statement = NULL;
    const char *tail = NULL;
    if (database.prepare(database.database, sql, -1, &statement, &tail) != 0 || !statement) return NULL;
    int ok = database.step(statement) == SQLITE_ROW;
    if (ok) {
        int64_t count = database.column_int64(statement, 0);
        memset(c, 0, sizeof *c);
        c->row = row;
        c->count = count > 0 && count <= CURVE_CELLS ? (unsigned)count : 0;
        for (unsigned i = 0; i < c->count; ++i) {
            c->exact[i] = database.column_int64(statement, 2 + 2 * (int)i) != 0;
            c->value[i] = (double)database.column_int64(statement, 1 + 2 * (int)i) / 1152921504606846976.0;
        }
    }
    database.finalize(statement);
    if (!ok || !c->count) return NULL;
    return &curves[curve_count++];
}

// Low-end and Top-end torque: every positive stock cell of the stock engine's
// full-throttle curves gets stock * (top + (low - top) * weight). Both sliders
// at x1.0 write the stock cells back exactly.
static int apply_curve(const Edit *edit, const CarTune *t, unsigned index) {
    static char sql[CURVE_CELLS * 64 + 400];
    uint32_t car = t->car;
    const char *slider = tune_sliders[index].property;
    double low = slider_ok[S_LOW_END] ? rounded(t->multiplier[S_LOW_END]) : 1.0;
    double top = slider_ok[S_TOP_END] ? rounded(t->multiplier[S_TOP_END]) : 1.0;
    int64_t rows[MAX_ROWS];
    double redline[MAX_ROWS];
    unsigned count = 0, too_many = 0;

    // Each curve row with its camshaft row (one each), whose stock redline places the taper.
    snprintf(sql, sizeof sql,
             "SELECT t.rowid, c.rowid, CAST(ROUND(c.RedlineRPM*%.0f) AS INTEGER), CAST(ROUND(c.TorqueCurveMaxRPM*%.0f) AS INTEGER) "
             "FROM List_TorqueCurve t JOIN List_UpgradeEngineCamshaft c ON c.TorqueCurveFullThrottleID=t.TorqueCurveID "
             "WHERE c.EngineID IN (SELECT EngineID FROM List_UpgradeEngine WHERE Ordinal=%u AND IsStock=1)",
             FIXED, FIXED, car);
    void *statement = NULL;
    const char *tail = NULL;
    if (database.prepare(database.database, sql, -1, &statement, &tail) != 0 || !statement) {
        logf_line("LIVETUNE: car %u %s: couldn't read the stock engine's torque curves.\r\n", car, slider);
        return 0;
    }
    int step;
    while ((step = database.step(statement)) == SQLITE_ROW) {
        if (count == MAX_ROWS) { too_many = 1; break; }
        int64_t cam = database.column_int64(statement, 1);
        // Remembered stock (Rev limit may have changed them); untouched rows are stock now.
        Stock *rpm = stock_for("List_UpgradeEngineCamshaft", "RedlineRPM", cam,
                               database.column_int64(statement, 2) / FIXED);
        Stock *max = stock_for("List_UpgradeEngineCamshaft", "TorqueCurveMaxRPM", cam,
                               database.column_int64(statement, 3) / FIXED);
        rows[count] = database.column_int64(statement, 0);
        redline[count++] = rpm && max && max->value > 0 ? rpm->value / max->value : 1.0;
    }
    database.finalize(statement);
    if (too_many || (step != SQLITE_DONE && step != SQLITE_ROW) || !count) {
        logf_line("LIVETUNE: car %u %s: %s; not changed.\r\n", car, slider,
                  count ? "the stock engine has more torque curves than expected" : "no torque curve for the stock engine");
        return 0;
    }

    unsigned written = 0, cells = 0, skipped = 0;
    double peak_stock = 0, peak_value = 0, idle_stock = 0, idle_value = 0;
    for (unsigned r = 0; r < count; ++r) {
        CurveStock *c = curve_for(rows[r]);
        if (!c) { log_line("LIVETUNE: couldn't read (or remember) a torque curve; tune not changed.\r\n"); break; }
        double redline_cell = redline[r] * (c->count - 1);
        size_t n = (size_t)snprintf(sql, sizeof sql, "UPDATE List_TorqueCurve SET ");
        unsigned set = 0;
        for (unsigned i = 0; i < c->count && n < sizeof sql; ++i) {
            if (!c->exact[i]) { ++skipped; continue; }
            if (!(c->value[i] > 0)) continue;  // the engine-braking cell past the curve stays
            double factor = top + (low - top) * low_end_weight(i, redline_cell);
            double value = factor == 1.0 ? c->value[i] : c->value[i] * factor;
            char number[64];
            sql_double(number, sizeof number, value);
            n += (size_t)snprintf(sql + n, sizeof sql - n, "%sv%u=%s", set++ ? ", " : "", i, number);
            if (!r && c->value[i] > peak_stock) { peak_stock = c->value[i]; peak_value = value; }
            if (!r && i == 0) { idle_stock = c->value[i]; idle_value = value; }
        }
        if (n >= sizeof sql) break;
        snprintf(sql + n, sizeof sql - n, " WHERE rowid=%lld", (long long)rows[r]);
        int64_t changed = 0;
        if (set && (!db_exec(sql, NULL) || !db_exec("SELECT changes()", &changed) || changed != 1)) break;
        cells += set;
        ++written;
    }
    if (written != count) {
        logf_line("LIVETUNE: car %u %s: torque curve update failed after %u of %u curves.\r\n", car, slider, written,
                  count);
        return 0;
    }
    logf_line("LIVETUNE: car %u %s (low x%.2f, top x%.2f): List_TorqueCurve %u cells in %u curve%s; 0 rpm %.3f -> %.3f, "
              "peak %.3f -> %.3f%s.\r\n", car, slider, low, top, cells, count, count == 1 ? "" : "s", idle_stock,
              idle_value, peak_stock, peak_value, skipped ? " (cells not exactly readable left stock)" : "");
    if (edit->sharers && !shared_noted) {
        int64_t cars = 0;
        edit_sql(sql, sizeof sql, "", edit->sharers, car);
        if (db_exec(sql, &cars) && cars > 1) {
            logf_line("LIVETUNE: note: those List_TorqueCurve rows are shared by %lld cars; all of them get this change.\r\n",
                      (long long)cars);
            shared_noted = 1;
        }
    }
    return 1;
}

static void save_tune(const CarTune *t) {
    wchar_t section[24], key[64], value[32];
    swprintf(section, ARRAYSIZE(section), L"Car%u", t->car);
    int any = 0;
    for (unsigned s = 0; s < tune_slider_count; ++s) {
        swprintf(key, ARRAYSIZE(key), L"%hs", tune_sliders[s].property);
        swprintf(value, ARRAYSIZE(value), L"%.2f", t->multiplier[s]);
        int stock_value = t->multiplier[s] == stock_setting(s);
        any |= !stock_value;
        WritePrivateProfileStringW(section, key, stock_value ? NULL : value, ini_path);
    }
    if (!any) WritePrivateProfileStringW(section, NULL, NULL, ini_path);  // back to stock: drop the car
}

// Boost on a naturally aspirated car only reaches a fitted turbo/supercharger upgrade.
static void note_aspiration(uint32_t car) {
    static const char *const tables[] = {"List_UpgradeEngineTurboSingle", "List_UpgradeEngineTurboTwin",
                                         "List_UpgradeEngineTurboQuad", "List_UpgradeEngineCSC", "List_UpgradeEngineDSC"};
    char sql[600], where[300];
    int64_t stock_rows = 0, any = 0;
    snprintf(where, sizeof where, STOCK_ENGINE, car);
    for (unsigned i = 0; i < ARRAYSIZE(tables); ++i) {
        int64_t n = 0;
        snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s WHERE IsStock=1 AND %s", tables[i], where);
        if (db_exec(sql, &n)) stock_rows += n;
        n = 0;
        snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s WHERE %s", tables[i], where);
        if (db_exec(sql, &n)) any += n;
    }
    if (!stock_rows)
        logf_line("LIVETUNE: car %u LiveTuneBoost: the stock engine is naturally aspirated; %s.\r\n", car,
                  any ? "only a fitted turbo or supercharger upgrade changes"
                      : "it has no turbo or supercharger rows, so nothing changes");
}

// Writes pending tunes. Only on the database's own thread.
static void apply_pending(void) {
    static int warned;
    if (!have_database || !tunes_dirty) return;
    if (GetCurrentThreadId() != database_thread) {
        if (!warned++) log_line("LIVETUNE: waiting for the database thread to apply a tune.\r\n");
        return;
    }
    if (!InterlockedExchange(&tunes_dirty, 0)) return;
    for (unsigned i = 0; i < MAX_TUNES; ++i) {
        CarTune *t = &tunes[i];
        if (!t->car) continue;
        int changed = 0;
        for (unsigned s = 0; s < tune_slider_count; ++s) {
            if (!InterlockedExchange(&t->dirty[s], 0)) continue;
            if (!slider_ok[s]) continue;
            int ok = 1;
            shared_noted = 0;
            for (unsigned e = 0; e < MAX_EDITS && edits[s][e].table && ok; ++e)
                if (column_ok[s][e])
                    ok = edits[s][e].kind == EDIT_CURVE ? apply_curve(&edits[s][e], t, s) : apply_edit(&edits[s][e], t, s);
            if (ok && s == S_BOOST) note_aspiration(t->car);
            changed |= ok;
        }
        if (InterlockedExchange(&t->save, 0)) save_tune(t);
        if (changed && driven_car) rebuild_car(t->car);  // nothing is driven yet at startup
    }
}

// Each column must exist in this database; a slider with none left is refused.
static void check_columns(void) {
    char sql[300];
    memset(mw_ok, 0, sizeof mw_ok);
    for (unsigned s = 0; s < tune_slider_count; ++s) {
        int usable = 0;
        for (unsigned e = 0; e < MAX_EDITS && edits[s][e].table; ++e) {
            const Edit *edit = &edits[s][e];
            snprintf(sql, sizeof sql, "SELECT %s%s%s FROM %s WHERE 0", edit->column, edit->at_least ? ", " : "",
                     edit->at_least ? edit->at_least : "", edit->table);
            column_ok[s][e] = db_exec(sql, NULL);
            if (!column_ok[s][e])
                logf_line("LIVETUNE DB: %s.%s not in this database; %s skips it.\r\n", edit->table, edit->column,
                          tune_sliders[s].property);
            if (column_ok[s][e] && s == S_MW && mw_needs[edit->mw]) {
                snprintf(sql, sizeof sql, "SELECT %s FROM Data_Car WHERE 0", mw_needs[edit->mw]);
                column_ok[s][e] = db_exec(sql, NULL);
                if (!column_ok[s][e])
                    logf_line("LIVETUNE DB: Data_Car.%s not in this database; %s skips %s.%s.\r\n", mw_needs[edit->mw],
                              tune_sliders[s].property, edit->table, edit->column);
            }
            if (s == S_MW) mw_ok[edit->mw] |= column_ok[s][e];  // other sliders blend only what MW can
            usable |= column_ok[s][e];
        }
        slider_ok[s] = usable;
        if (!usable) logf_line("LIVETUNE DB: %s has nothing to change; that slider is disabled.\r\n", tune_sliders[s].property);
    }
}

void tunes_database(const struct FH2PluginDatabaseFrame *frame) {
    if (!frame || frame->size < sizeof *frame || !frame->database || !frame->prepare || !frame->step ||
        !frame->finalize || !frame->column_int64)
        return;  // nothing changed, so not a failure
    database = *frame;
    have_database = 1;
    database_thread = GetCurrentThreadId();
    check_columns();
    // A new merge starts from stock values: forget stock, re-apply tunes.
    stock_count = 0;
    curve_count = 0;
    for (unsigned i = 0; i < MAX_TUNES; ++i)
        if (tunes[i].car)
            for (unsigned s = 0; s < tune_slider_count; ++s) tunes[i].dirty[s] = tunes[i].multiplier[s] != stock_setting(s);
    InterlockedExchange(&tunes_dirty, 1);
    apply_pending();
    log_line("LIVETUNE DB: merged database connection received.\r\n");
}

void tunes_apply(uint32_t car) {
    driven_car = car;
    apply_pending();
}

unsigned long tunes_database_thread(void) {
    return database_thread;
}

float tunes_get(unsigned index) {
    CarTune *t = index < tune_slider_count ? tune_for(driven_car, 0) : NULL;
    return t ? t->multiplier[index] : index < tune_slider_count ? stock_setting(index) : 1.0f;
}

// Read-only lookup for any thread (UI hooks): volatile reads of the tune table,
// no database access. A slot's car id is set after its multipliers, and slots
// are never freed, so a match always has initialized values.
float tunes_multiplier_for(uint32_t car, unsigned index) {
    if (!car || index >= tune_slider_count) return 1.0f;
    for (unsigned i = 0; i < MAX_TUNES; ++i) {
        const volatile CarTune *t = &tunes[i];
        if (t->car != car) continue;
        float value = t->multiplier[index];
        return value == value ? value : stock_setting(index);  // NaN never, but be safe
    }
    return stock_setting(index);
}

int tunes_set(unsigned index, float multiplier) {
    uint32_t car = driven_car;
    if (index >= tune_slider_count) return 0;
    CarTune *t = car ? tune_for(car, 1) : NULL;
    if (!t) {
        static int logged;
        if (!logged++) log_line("LIVETUNE: no car seen yet (vehicle updates haven't run); slider change refused.\r\n");
        return 0;
    }
    if (have_database && !slider_ok[index]) return 0;
    if (t->multiplier[index] == multiplier) return 0;
    t->multiplier[index] = multiplier;
    InterlockedExchange(&t->dirty[index], 1);
    InterlockedExchange(&t->save, 1);
    InterlockedExchange(&tunes_dirty, 1);
    apply_pending();  // now if we're on the database thread, otherwise on the next vehicle update
    return 1;
}

// tunes.ini: one [Car<Id>] section per tuned car, <property>=<multiplier>.
// Values outside a slider's range are ignored.
static void load_tunes(void) {
    static wchar_t names[16384];
    wchar_t key[64], text[32];
    unsigned loaded = 0;
    DWORD length = GetPrivateProfileSectionNamesW(names, ARRAYSIZE(names), ini_path);
    for (const wchar_t *name = names; length && *name; name += wcslen(name) + 1) {
        wchar_t *end = NULL;
        if (_wcsnicmp(name, L"Car", 3)) continue;
        unsigned long car = wcstoul(name + 3, &end, 10);
        if (!car || car > 0xffffffffu || *end) continue;
        CarTune *t = NULL;
        for (unsigned s = 0; s < tune_slider_count; ++s) {
            const SliderSpec *spec = &tune_sliders[s];
            swprintf(key, ARRAYSIZE(key), L"%hs", spec->property);
            if (!GetPrivateProfileStringW(name, key, L"", text, ARRAYSIZE(text), ini_path)) continue;
            float value = (float)wcstod(text, &end);
            if (end == text || !(value >= spec->min && value <= spec->max)) {
                logf_line("LIVETUNE: tunes.ini [%ls] %s=%ls ignored (outside %.2f-%.2f).\r\n", name, spec->property,
                          text, spec->min, spec->max);
                continue;
            }
            value = (float)(int)(value / spec->step + 0.5f) * spec->step;
            if (value == stock_setting(s)) continue;
            if (!t && !(t = tune_for((uint32_t)car, 1))) break;
            t->multiplier[s] = value;
            t->dirty[s] = 1;
        }
        loaded += t != NULL;
    }
    if (loaded) logf_line("LIVETUNE: %u saved tunes loaded from tunes.ini.\r\n", loaded);
}

void tunes_init(TabLog log, TuneRebuild rebuild, const wchar_t *plugins_directory) {
    log_line = log;
    rebuild_car = rebuild;
    swprintf(ini_path, MAX_PATH, L"%ls\\..\\tunes.ini", plugins_directory);
    load_tunes();
}
