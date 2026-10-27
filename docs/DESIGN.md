# LiveTune — in-game tuning menu (design)

> Historical design log from development, kept for reference. "LiveTune" was the working name
> of Enhanced Tuning. Some plans here changed before release: tunes are saved in `tunes.ini` (not
> per-car JSON files), the mod folder is `mods\Enhanced Tuning\`, the menu is the native
> ENHANCED TUNING tab (option B below; the overlay was never built), and most sliders listed as
> "to verify" or "research" were implemented, some on different columns (weight, brakes, grip,
> top speed); the drivetrain-type and flywheel sliders were not. See `src/tunes.c` for the final list.
> The F9 car snapshot mentioned under the research backlog was a development-only tool and is not
> in the release.

## What we know works (2026-10-27)

- Extra Options hands plugins its merged in-memory game database (needs any active
  mod with a `database.json`; LiveTune ships an empty one).
- Editing a car's row and respawning (pause → My Cars → Get in Car) rebuilds the car
  from the database. Verified: downforce (seen in memory), torque (felt, x3).
- Not working yet: weight (`CurbWeight` edit didn't reach the respawned car).
- Editing the car's memory directly does nothing: the physics is built at spawn.

So the menu is: **pick values → write them to the database → respawn the car.**

## Ground rules: self-contained and non-destructive

- Everything ships in one mod folder (`mods/LiveTune/`). Deleting it removes LiveTune completely.
- No game file is replaced. UI changes are inserted into whatever version of a file
  the game loads, in memory, so other mods' edits to the same screen survive.
- No code is patched on disk. In-memory patches only apply when the expected bytes and
  structures are found exactly; otherwise LiveTune does nothing and logs why.
- Data changes are additive (`strings.json`) or in-memory (the merged database).
  Tunes are LiveTune's own files.
- Every feature fails closed: if one piece can't be applied, the rest keep working
  and the game stays stock.

## Pieces

### 1. Tune engine (plugin core)

A tune is a list of `(table, row, column, value)` edits for one car, stored as
`mods/LiveTune/tunes/<CarId>.json`, e.g.

```json
{ "car": 2267, "sliders": { "torque": 1.5, "downforce_front": 2.0, "brakes": 1.2 } }
```

Sliders map to database columns through one table in the plugin:

| Slider | Table / column | Scope | Status |
|---|---|---|---|
| Horsepower / torque | `Data_Car.GameTorqueScale` (scales the whole curve, so hp and torque move together; menu shows estimated hp) | this car | ✅ works |
| Rev limit | `Data_Car.SimRedlineAngVel` / engine tables | this car | to verify |
| Turbo boost | `Data_Engine.StockBoost-bar` | engine (shared by cars with that engine) | to verify |
| Rev speed (flywheel) | `Data_Engine.MomentInertia` | engine | to verify |
| Drivetrain (RWD/AWD/FWD) | `Data_Drivetrain.DrivetypeID` | drivetrain row | to verify |
| Top speed limiter | `Data_Car.SpeedLimiterID` | this car | to verify |
| Downforce F/R | `Data_Car.BodyAeroForwardDownforceFront/Rear` | this car | ✅ reaches car |
| Drag | `Data_Car.BodyAeroLongitudinalDrag` | this car | to verify |
| Steering lock | `Data_Car.SteerMaxAngle` | this car | to verify |
| Brake force | `List_UpgradeBrakes.GameFrictionScaleBraking` (Ordinal = car) | this car | to verify |
| Tire grip | `List_TireCompound.TireFricScale0/1` | **shared by every car on that compound** — clone the row per car | to verify |
| Weight | unknown source (not `Data_Car.CurbWeight`) | — | research |

Sliders are multipliers on the stock value, so tunes survive mod updates and
"reset" is just 1.0. Stock values are read once and kept, so repeated edits
never compound.

Tunes are applied twice:
- **at startup**, inside Extra Options' database callback (within its contract),
  so a saved tune is there every session without touching anything;
- **live**, when you change a slider, through the kept connection, then respawn.

### 2. The menu

Options, roughly in order of effort:

| | Looks | Effort | Risk |
|---|---|---|---|
| **A. Overlay** (Dear ImGui drawn on Azimuth's D3D12 swapchain) | its own panel over the game | medium | hooking Present under a translation layer; must not fight Azimuth's overlay |
| **B. Native page** (rewrite the game's XAML like Extra Options' video menu) | looks like a real FH2 menu | high (≈30 game addresses, resource-reader hook that Extra Options already owns) | breaks on game/EO updates; conflicts with EO's hook |
| **C. Ask Extra Options for a plugin settings API** | native | low for us, depends on the EO author | timing out of our hands |

Recommendation: **A first** (fast to iterate, full sliders, controller nav),
and raise **C** with the Extra Options author for a native look later.

#### Branding the game's own Tuning menu

The Tuning menu is `media/UI.zip → Scenes/Anthem/A_TuningOptions.xaml`: 9
categories (Tires … Differential), each a panel whose sliders bind to native
`Tuning.*` properties. Extra Options' loader can override a ZIP member with a
loose file in a mod's `media/UI/Scenes/Anthem/`, so:

- **A text line** ("+ Enhanced Tuning — press F6") in that screen is a XAML-only
  edit shipped as a mod. Good odds.
- **A real 10th category with working sliders** needs the native category list
  and new native properties (what Extra Options does for its video/HUD pages).
  Possible, much more work. **This is the chosen goal** ("Enhanced Tuning" tab,
  ideally first, left of Tires). Order of work: an empty tab at the end → sliders
  → try moving it first (the game may assume tab 1 = Tires, 2 = Gearing).
  The tab table is filled at startup by `game+0x151fa0` (9 entries pairing
  `StackPanel_N_*` with `VisualState_*`).

#### Tab table (reverse-engineered)

- Table at `game+0x3540a10`, 9 entries × 0x50 bytes, packed against another table
  at `0x3540ce0` (no room to append in place).
- Entry: `+0x00` state name ptr, `+0x08` panel name ptr, `+0x10..0x2f` optional
  secondary-state callback (`+0x28` impl ptr, 0 = none), `+0x30..0x4f` label callback.
  Tires' label is a string-table ID resolved from `"IDS_Tires"` (`game+0x171cb0`).
- Readers, all indexing `table[selected_tab]` with **no bounds check**:
  - `game+0xe90d60` page set-up: loops over the table, adding one tab per entry;
    count is `lea r14d, [rdi+0xa]` with `rdi = -1` at `game+0xe90d67` (9 tabs).
  - `game+0xe93f73` focus: `table[tab].panel`.
  - `game+0xe94dfa` tab change: GoToState(`table[tab].state`), then the secondary state.
- Plan (in memory, guarded): build a 10-entry copy near the image (ours + the 9
  originals, which keep pointing at their original callback storage), redirect those
  three RIP-relative references, bump the count to 10, and add our panel/state to the
  Tuning XAML as it loads. Open questions: making our label callback; hooking the XAML
  load without fighting Extra Options' own resource hook; code that assumes tab 2 = Gearing.
- The line can point at the overlay, so the Tuning screen becomes the natural
  place to open it.

Controls (A): open/close with a hotkey (keyboard F6, controller: hold View + D-pad Down),
D-pad / arrows to move, left/right to change, A / Enter = **Apply & respawn**,
Y = reset slider, B / Esc = close. Game input should be ignored while it's open.

### 3. Respawn

v1: after Apply, the menu tells you to Get in Car. v2: trigger the game's own
car-reload flow (Extra Options does this by editing `pause.xml`'s state flow with
`_fh2_car_unload` / reload nodes) so Apply respawns automatically.

### 4. Research backlog

- Weight: find what feeds `car+0x1360` (owned-car record? upgrade MassDiff? boot cache).
- Verify each slider: edit → respawn → F9 dump or feel.
- Tire compound cloning without breaking the upgrade shop.
- Truly live (no respawn) sliders, only if we ever find the live physics copy.
