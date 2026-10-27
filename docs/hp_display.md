# Where the stat screens' horsepower comes from (2026-10-27)

Static research on `forza_durango_release_final.exe` (FH2 1.0.0.64). All addresses are RVAs
(game+X). Confidence: **high** = read directly in code; **medium** = inferred.

## TL;DR

* Every car-stat panel binds `SimPeakPower` / `SimPeakTorque` of **one UI view model**, the
  "car stats" item (vtable **0x2dd1db8**, base ctor 0x15d14c0). XAML, `UI.zip`
  `Scenes/Anthem/A_CarSelect.xaml` (also BarnFinds, LoadSaveTuning, MarketplaceCarSelect,
  SelectUpgradePart, UpgradeCarConfirmPopup and the WPF F111/F453/F532/F538/F552/F600/F603/F703):
  `Content="{Binding SimPeakPower, ConverterParameter=power, Converter={StaticResource A_NumUnitConverter_PerformanceStats}}"`
  with `DataContext="{Binding SelectedItem, ElementName=List}"`. (high)
* The view model's floats: **vm+0x98 SimPeakPower, vm+0x9c SimPeakTorque**. Getters 0xcc9460 /
  0xcc95e0, registered by 0xcdf430 / 0xcdf590. These are the only UI properties with those
  names (the only UI-side reference to the strings). (high)
* **Units: Data_Car units.** Power is in 100 W and torque in 100 N·m. The XAML `NumUnitConverter`
  (`param=power|torque`) converts to hp / lb-ft (or kW / N·m). Veyron SS (2286):
  `Data_Car.SimPeakPower` 8819.2 = 881.9 kW = **1183 hp**. `SimPeakTorque` 15.0 = 1500 N·m. (high
  for the units, from the DB: SimPeakPower / SimPeakAngVel = 13.16, torque at peak power.)
* The view model is filled by exactly two functions. Each copies 14 stats and nothing else (high):

| fill | source | callers |
|---|---|---|
| **0xccafe0** `(vm, extra)` | by **Data_Car Id `[vm+0xa8]`**: builds a temporary stats object 0xaf11e0 (runs `SELECT … FROM Data_Car WHERE Id=`, the physics builder 0x1056210 and the stats sim 0x1044da0 into `[0x365d700]`), then its getters: +0x38 power (0xbada10), +0x3c torque (0xa4ec00) | ctor 0xcc0a90 (← 0xcbe570 ← 0xcbff90 ← 0x824040, 0xeada30), vtable slot 0x98 = 0xcd9b00 |
| **0xccb110** `(vm, TRef<CCarInGarage>*)` | the **garage car's cached stats**: `CCarInGarage+0x278` power (getter 0xa9e010), `+0x27c` torque (0xa9e030). The callee releases the ref | ctor 0xcc0b00 (← 0xeadab0 ← 0xeaed10, which gets the career car via 0x2dca30), vtable slot 0xa0 = 0xcd9b20 |

* `CCarInGarage+0x278/+0x27c` are written only by the stats rebuild **0x2cfa50** (copied from
  `[0x365d700]+0` / `+8` after the loader 0x2d0160 runs the same stats sim). That's the function
  that also `UPDATE`s the garage row when `+0x2bc != 0` (swap.md). The row columns
  `NewProfile_Career_Garage.SimPeakPower/…` are written from these fields, but the UI **doesn't read
  the row**. The only other reader of the row's SimPeakPower, 0x2a4040, is the cloud
  change-tracking sync (← 0x28dad0, `ChangeTracking WHERE TransactionId =`). It isn't UI. (high)
* `Data_Car.SimPeakPower` (the column LiveTune scales) is **not** what the stat bars show. The
  string is read by 0x1059b80 (a Data_Car info row: Year, PI, SimPeakPower, CurbWeight, MakeId …,
  for the car lists / filters / sorting) and 0x37bfd4. (medium: I didn't trace their UI use. They
  may drive list sorting/filters, not the stat bars.)

## Does the same value feed PI / class or anything non-display?

* **The view model is display-only.** PI (+0x90), ClassID (+0x78), ratings (+0x70..+0x8c, +0xa0),
  CurbWeight (+0x7c) and WeightDistribution (+0xa4) are separate fields, filled from separate
  getters. The `…Change` properties (0xcc9390 etc.) compare two view models' fields for the
  upgrade/tuning comparison arrows. PI and class are computed in the stats sim (0x1044da0 /
  0x2d0160), upstream of the view model, and never read back from it. (high for the fill, medium
  that nothing else reads vm+0x98)
* **`CCarInGarage+0x278` is not display-only**: 0x2cfa50 writes it into the saved garage row. So
  the hook must not touch it. It doesn't: it changes only the view model's copy, after the fill
  has run.

## The hook (`src\hp_display.c`)

* Entry hooks (entry_hook.c) on 0xccafe0 (prologue `40 53 48 81 EC 90 00 00 00`) and 0xccb110
  (`48 89 54 24 10 41 56 48 83 EC 30`). Both are 16-byte aligned and checked byte for byte;
  anything else means no hook. The handler calls the original through the trampoline, then
  `vm+0x98 *= m`, `vm+0x9c *= m` with `m = tunes_multiplier_for(car, 0)` (Horsepower).
* Car id: for 0xccafe0, `[vm+0xa8]` (the id the fill itself computes for). For 0xccb110,
  `[CCarInGarage+0x40]`, read **before** calling the original (it releases the ref), and only if
  `[car] == game+0x2c5f818` (CCarInGarage vtable). Note `vm+0xa8` isn't refreshed by the
  vtable-0xa0 setter, so it's not used there.
* `m == 1`, an untuned car, or an unknown car: nothing changes. The first time it scales a value
  for a car, it logs one line (up to 16 cars):
  `LIVETUNE HP: car 2286 stat screen x20.00: power 8819.2 -> 176384.0 (1183 -> 23653 hp), torque 15.000 -> 300.000.`
* Off switch: `LiveTune.ini` `[Display] ScalePower=0`.

## Open question: is the sim value already tuned? (check in game)

Both sources come from the game's own **stats sim over the in-memory DB**, the same physics
builder that the live car uses. The torque sampler (CCarDynamics sub-object +0x3a0, vtable
0x2eb5e18, slots 0xa30/0xa38 → 0x12e18f0 / 0x12e1880 → 0x12e13f0) is linear in the built torque
curve. So in principle a garage car whose stats were computed **after** LiveTune scaled
`List_TorqueCurve.TorqueScale` would already show the tuned power. The hook would then count the
multiplier twice.

The facts point the other way, but I couldn't settle it statically:
* In a test session log, the 20x tune for 2286 was written (lines 59-61) **before** the career
  CCarInGarage was built and its stats sim ran (lines 114-117). Yet the stat screen showed about
  2200 hp (≈ 1.86 × 1183), not about 23 600. So the sim doesn't scale linearly with TorqueScale
  (some clamp or normalization I didn't find), or the 2200 was seen in another state.

**In-game check:** set Horsepower to x2 on an un-upgraded car, open My Cars, and read the
`LIVETUNE HP:` line. If the "before" hp is about stock (1183 for 2286), the hook is right. If it's
already about 2× stock, the sim includes the tune: set `ScalePower=0`, and the display needs no
hook. If it's somewhere in between (like 1.86 at 20x), the sim is clamping. Then the real car is
probably clamped too, and that's worth knowing for the Horsepower slider itself.

Other notes:
* Only the Horsepower multiplier is applied. Low-end / Top-end torque and Boost also change the
  curve, and they aren't included.
* The career car's CCarInGarage stats are cached (garage+0x50) until a swap or a LiveTune
  re-prepare. The hook scales with the multiplier at fill time, so the display follows the slider
  the next time the screen is filled.
