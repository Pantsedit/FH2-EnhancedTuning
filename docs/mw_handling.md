# NFS Most Wanted (2005)-style handling on Forza Horizon 2: what it would take (2026-10-27)

This is static research behind Enhanced Tuning's **MW Handling** slider. Nothing here was run
in-game when it was written. The FH2 column names below were all checked against the game
database (`gamedbRC.slt`, FH2 1.0.0.64), opened read-only from a local install. **What each FH2
column does is inferred from its name and its values.** Only the levers DESIGN.md marks as proven
(GameTorqueScale, downforce F/R) were known to reach the car at the time.

**Inspiration, not content.** The "MW feel" below is a summary of public and community research
into how Need for Speed: Most Wanted (2005) handles (see CREDITS.md). No Need for Speed file,
code or data is part of this project, and none is needed to build or use it. The MW Handling
slider only rescales Forza Horizon 2's own database values toward the targets in section 4.

---

## 1. What defines "MW handling"

In short, MW is a grip racer with **very high grip that doesn't fall off past the limit**,
**strong downforce at speed**, **lazy yaw**, an **automatic rear stability boost that the handbrake
or a drift switches off**, **steering lock that collapses with speed**, and a **tail-happy,
wheelspin-prone launch**. The codes are used in the mapping tables below.

| # | Behaviour | In short |
|---|---|---|
| G1 | **Huge grip** | Grip levels well above a road-car sim, scaled up further with speed. |
| G2 | **No fall-off past peak** | Lateral grip saturates instead of dropping off at large slip angles; load sensitivity is degressive. |
| G3 | **Grip vs slide hysteresis** | Once traction is lost, the tire stays in a sliding mode until slip drops again, so wheelspin "latches". |
| A1 | **Massive downforce** | Downforce grows with speed and reaches more than the car's weight at high speed. |
| A2 | **Drag** | Quadratic drag, stronger off-throttle. |
| Y1 | **Lazy yaw** | Only part of the tires' yaw moment is applied and yaw inertia is raised, so cars rotate lazily. |
| Y2 | **Speed stability boost** | Extra rear grip while rotating at speed (up to about a third more), reduced by the handbrake or a drift. |
| D1 | **Drift state** | A drift mode with yaw damping and countersteer-driven rear friction gives long, held, self-catching drifts. |
| H1 | **Handbrake** | A very strong handbrake that locks the rears almost instantly. |
| B1 | **Rear-first brake lock** | Rear brakes lock first. |
| S1 | **Steering lock collapses with speed** | About 40° at rest, around 10° at 80 mph and about 5° at 120 mph; fast steering rate at low speed. |
| S2 | **Countersteer assist** | Steering into a rear slide allows more lock. |
| L1 | **Tail-happy launch** | Reduced driven-wheel grip at low speed, so launches spin the wheels. |
| L2 | **Locked diff at low speed** | Driven wheels effectively tied together below about 40 mph. |
| C1 | **Walls and contacts** | Little bounce and high yaw inertia in contacts: slap the wall, stay pointed, scrape along. |
| I1 | **Input layer** | Auto-brake and auto-reverse at low speed. |

The **core feel**, in order of importance:
1. G1 + A1 (glued, high grip at speed).
2. Y1 + Y2 (stable, lazy rotation).
3. H1 + D1 (handbrake → held drift).
4. S1 (tiny lock at speed).
5. L1 (wheelspin launch).

---

## 2. Mapping MW → FH2 database

Legend:
- **DB** = a direct DB lever.
- **≈DB** = approximated by one or more columns.
- **HOOK** = needs a runtime hook into FH2's sim (feasibility unknown).
- *(proven)* / *(untested)* / *(semantics unknown)* = how sure we are of the lever itself.

Typical FH2 values come from the DB. Sample cars: E46 M3 Id 383, SL65 1090, 911 GT2 '12 1539,
Gallardo LP570 1322.

### 2.1 Grip (G1, G2, G3)

| MW | FH2 lever | Kind | Notes |
|---|---|---|---|
| G1 overall grip level | `List_UpgradeCarBodyChassisStiffness.Front/RearLat/LongFrictionScale` (rows per car body, `CarbodyId IN (SELECT CarBodyID FROM List_UpgradeCarBody WHERE Ordinal=<car>)`) | **DB** *(untested; already a LiveTune slider)* | Stock 0.955–1.085. **Per car**: no car body is shared by two cars (checked). The best per-car grip knob. |
| | `List_TireCompound.TireFricScale0/1` (0.8625 / 1.2, interpolated by `TireFricWidth0/1` = 150 / 650 mm, so a tire-width scale) and `TorqueFreeLatFrictionScale` (1.6–1.8) | ≈DB *(untested)* | **Shared by every car on a compound.** Compound 7 (DOT Competition) is used by 373 upgrade rows; 9 (Offroad) by 391. |
| | `List_TireFrictionCurve.FrictionScale` (e.g. 1.09 / 0.872) | ≈DB *(untested)* | Shared per compound. |
| | `Combo_TireBrandCompound.FrictionScale` | DB | 0.991–1.0. Not worth using. |
| | `Data_Car.Lat/LongAccel/LongBrake{Front,Rear}ScalarMax/Clamp` (Max 0.22–3.74, Clamp = 5×Max) | *(semantics unknown)* | Possibly caps on a load- or aero-based friction scalar. **Risk: these could clamp any big grip increase.** Test them if raising friction scales stops helping. |
| G1 global speed shaping (GripVsSpeed ×1.2) | `List_TireCompound.AffectCurveSpeedAffectFrictionID` → `List_TireAffectCurve` (11 points over input 0..1.5, e.g. 1.37 → 1.0) | ≈DB *(semantics unknown)* | The input is probably a speed or slip-speed measure. Shared per compound. |
| **G2 no fall-off past peak** | `List_TireFrictionMultiCurve` (lateral MaxSlip about 50, so likely slip-angle degrees or a 100-step axis; longitudinal MaxSlip about 1.1 slip ratio), with `TireFrictionCurveID0/1` = low-load and high-load curves → `List_TireFrictionCurve.v0..v99` | **DB** *(untested)* | FH2 lateral curves peak around index 10–15 and fall to 0.84–0.88 at the end. **Setting every value after the peak to the peak value gives MW's saturating curve.** It is the single most "MW" data edit available. It is shared per compound, so clone the rows (see §3.3). |
| G2 load sensitivity | Curve0 vs curve1, `MinLoadCurve` / `MaxLoadCurve` / `LoadClamp` | ≈DB | Already degressive (the high-load curve's FrictionScale is 0.872), like MW. Leave it. |
| G3 grip/slide latch | — | HOOK | FH2 uses a continuous slip curve. A latch is a different tire model. Flattening the curve (G2) removes the snap that a latch would otherwise cover up. |

### 2.2 Aero (A1, A2)

| MW | FH2 lever | Kind | Notes |
|---|---|---|---|
| A1 downforce amount | `Data_Car.BodyAeroForwardDownforceFront/Rear` | **DB (proven reaches car)** | Range −500…+575. **56 of 391 cars have rear lift (negative), E46 M3 among them.** Multiplying a negative value makes lift worse, so the preset must set a floor, not just multiply. Also `List_UpgradeCarBodyRearBumper.BodyAeroForwardDownforceFront/Rear` and the wing `List_AeroPhysics.Downforce0/1`. |
| A1 linear in speed | — | HOOK | FH2 downforce is almost certainly ∝ v². A fixed coefficient can match MW at one speed only: too little at low speed and too much at high speed. Pick a match speed of about 90–110 mph. |
| A1 aero spring-load effect | `Data_Car.TireAeroHackShouldApply/MinSpringLoad/MaxSpringLoad` (1 / 1 / 1.55) | *(semantics unknown)* | Possibly limits how much aero load the suspension sees. Leave it. |
| A2 drag | `Data_Car.GameDragScale` (0.7–1.25), `BodyAeroLongitudinalDrag` | DB *(untested; LiveTune slider)* | MW drag is quadratic too, so this is a fair match. |
| A2 off-throttle ×2 | — | HOOK | Approximate with engine braking (`List_TorqueCurve.ZeroThrottleTorqueScale`), shared per curve. |

### 2.3 Yaw and stability (Y1, Y2)

| MW | FH2 lever | Kind | Notes |
|---|---|---|---|
| Y1 yaw inertia ×2 (TENSOR_SCALE) | `List_UpgradeCarBodyWeight.BlockDimX/Y/Z` (inertia box; E46: 1.958 × 0.849 × 4.718 m) | ≈DB *(untested)* | Box yaw inertia ∝ X²+Z². ×1.41 on X and Z doubles Iyy but also raises roll and pitch inertia. **We don't know whether BlockDim is also used for collision or the camera.** |
| Y1 YAW_SPEED 0.25 (only ¼ of the tire yaw moment applied) | — | HOOK | No FH2 equivalent. The inertia increase is the closest DB proxy, but it slows response; it doesn't scale the tire moment. |
| Y2 rear stability boost (+≤35% rear grip at speed) | Static: raise `RearLatFrictionScale` above front (a stable balance). `Data_Car.FixListingRearFricScale` (1.35 / 1.45), with ramps `FixListingNormSlip0/1` (0.5 / 1) and `FixListingSteerAngle0/1` (0.5 / 1) | ≈DB *(semantics unknown)* | FixListing looks like FH2's own rear-friction boost, ramped by normalised slip and steer angle. 1.35 = MW's 1 + 0.35 cap, which may be a coincidence. **Test FixListingRearFricScale first: it may be the closest native equivalent of Y2.** |
| Y2 switched off by the handbrake or a drift | — | HOOK | Needs the handbrake input in the sim loop. |
| Forza's STM assist | `AutoSteerOverrides.STMSlipMin/Max` (1.5 / 3.0, three shared rows), `Data_Car.AutoSteerOverrideID` | DB *(only with the STM assist on)* | Only matters with assists on. |

### 2.4 Handbrake, brakes, drift (H1, B1, D1)

| MW | FH2 lever | Kind | Notes |
|---|---|---|---|
| H1 handbrake strength (×10, instant rear lock) | `List_UpgradeBrakes.BiasHandbrake` (1.5 or 2.0, per car and level) | ≈DB *(semantics unknown)* | Probably handbrake torque relative to the brakes. Try ×2–3. |
| B1 brake strength (2.4 g) | `List_UpgradeBrakes.GameFrictionScaleBraking` (1.0–1.2) | DB *(untested; LiveTune slider)* | Grip limits braking more than torque. Raise G1 first. |
| B1 rear-first lock | `List_UpgradeBrakes.BrakeBiasSlider` (0.5) and `ReleasePointABSRear*` | ≈DB *(semantics unknown)* | The bias is probably the tuning default. Rear-biased brakes make the car unstable under braking, unlike MW, where the stability assist hides it. **Don't change it.** |
| D1 drift state (yaw damping, countersteer-driven rear friction) | Partial: flattened post-peak curves (G2), plus `SteerAngVelCountersteer` (105, or 1000 on E46), plus rear LSD | HOOK for the real thing | The DriftStabilizer yaw damping and the countersteer → rear-grip coupling are runtime logic. |

### 2.5 Steering (S1, S2)

| MW | FH2 lever | Kind | Notes |
|---|---|---|---|
| S1 lock at rest 40° | `Data_Car.SteerMaxAngle` (27–50, median 42) | **DB** *(untested)* | Already close to MW. |
| S1 lock collapses with speed | `Data_Car.SteerSpeedSensitiveMinMaxAngle` (4–5), `SteerSpeedSensitiveFastSpeed` (100–120), `SteerSpeedSensitiveSlowSpeed` (1), `SteerSpeedSensitiveMaxGees` (1), `SteerSpeedSensitiveFastRateScale` (1), `SteerMaxAngleFiltered` (12–50) | ≈DB *(semantics partly guessed)* | FH2 apparently lerps the max angle down to MinMaxAngle by FastSpeed (unit unknown, likely mph or km/h) and/or limits lock to MaxGees. MW targets: 5.5° at 120 mph, 10° at 80 mph. Suggested values: MinMaxAngle 3–3.5, FastSpeed about 120 (mph), MaxGees left as is. **These may only apply to pad steering with the "normal" (assisted) steering option.** |
| S1 steer rate 180°/s, falling to 30% with speed | `SteerMaxAngVelTurning` (70–105), `SteerMaxAngVelStraighten` (95–105), `SteerAccelTimeToMaxRate` | DB *(untested)* | MW is fast at low speed. Raise Turning to about 150. Use FastRateScale (1) to reduce it at speed, e.g. 0.5. |
| S2 countersteer up to 45° | `SteerAngVelCountersteer` (105, or 1000 on 4 cars), `SteerAngVelDynFindPeak` | ≈DB | Set Countersteer 1000, like the E46 M3 row. |
| Steering range grows with sustained input, widens under braking | — | HOOK | |

### 2.6 Launch and drivetrain (L1, L2)

| MW | FH2 lever | Kind | Notes |
|---|---|---|---|
| L1 power | `Data_Car.GameTorqueScale` | **DB (proven)** | |
| L1 low-speed traction cut (pilot 0.85 below 30 mph) | `List_TireCompound.TorqueFreeLongFrictionScaleAccel0/1` (1.3 / 1.15–1.22) at `…AccelSpeed0/1` (5 / 15, probably m/s) | ≈DB *(semantics unknown; shared per compound)* | FH2 already varies accel friction with speed, but the other way round (more grip at low speed). Lowering Accel0 toward Accel1 or below gives an MW-style wheelspin launch. |
| L1 TCS | `Data_Car.AssistsTCSSlipDefTakeoff` (33), `AssistsTCSSlipDefMoving` (0.5) | DB *(only with TCS on)* | |
| L1 clutch snap, burnout fishtail, perfect launch | — | HOOK | |
| L2 locked diff | `List_UpgradeDrivetrainDifferential.RearLimitedSlipTorqueAccel/Decel` (E46 stock 1000/125, race 1500/750), `RearToqueSplit` (AWD) | DB *(untested)* | Raise accel to the race-diff value (fairly locked). Leave decel moderate so lift-off rotation stays. Rows are per DrivetrainID; check whether they're shared. |

### 2.7 Collisions, input, mass

| MW | FH2 lever | Kind |
|---|---|---|
| C1 no bounce / ×15 yaw inertia in contact | — | HOOK (collision response) |
| I1 auto-brake, auto-reverse | — | HOOK (FH2 has its own auto-reverse) |
| MASS ~1300 kg | `List_UpgradeCarBodyWeight.Mass` / `InitialMass` (rows per car body and level; E46 1549 → 1264 kg race) | DB *(untested)* |

**Side finding for the DESIGN.md weight backlog:** `Data_Car.CurbWeight` is a display value
(15.49 means 1549 kg). The physics mass is very likely `List_UpgradeCarBodyWeight.Mass`
(selected by the weight upgrade level), together with `CMHeight`, `CMBackFront` and `BlockDim*`.
That would explain why the CurbWeight edit didn't reach the car.

---

## 3. Proposal: an "MW Handling" preset for LiveTune

### 3.1 Approach

- Add one preset with **intensity 0–100%**. It lerps every lever between stock (0%) and the MW target (100%).
- Use the existing `Edit` machinery: per-row stock memory, multiply, write, rebuild.
- It needs two features `tunes.c` doesn't have yet:
  1. **absolute targets and floors**, e.g. `MAX(stock, target)` for downforce, and `SET v_i = peak` for curves;
  2. **cloned tire rows** (§3.3).
- Scale the grip target by class rather than one global multiplier. MW cars all live at "glued"
  grip. FH2's `Data_Car.SimLatGees60MPH` averages, by ClassID: 0.75 (D), 0.79 (C), 0.85 (B),
  0.89 (A), 0.96 (S1), 1.11 (S2), 1.45 (X).
- **Per car:** `gripMul = clamp(target / SimLatGees60MPH, 1.0, 1.8)` with `target ≈ 1.5` (not
  MW's 3.8 g, which FH2's AI, camera and track design wouldn't survive). This flattens
  the class spread toward MW's "everything grips" while keeping the order.

### 3.2 Preset contents (Tier 1 = per-car rows, safe to try first)

| Lever | Value at 100% | MW behaviour |
|---|---|---|
| `List_UpgradeCarBodyChassisStiffness` Front/Rear Lat+Long FrictionScale | × gripMul; rear ×1.05 more than front | G1, Y2 (stable balance) |
| `Data_Car.BodyAeroForwardDownforceFront/Rear` | `max(stock×2, floor)`, with floor ≈ class median of S1 (94 / 173) and rear ≥ 1.6× front | A1 (glued at speed, kills rear lift) |
| `Data_Car.FixListingRearFricScale` | 1.35 → 1.5 | Y2 (test first, semantics unknown) |
| `Data_Car.SteerSpeedSensitiveMinMaxAngle` | → 3.25 | S1 |
| `Data_Car.SteerMaxAngVelTurning` / `SteerAngVelCountersteer` | → 150 / 1000 | S1 rate, S2 |
| `List_UpgradeBrakes.BiasHandbrake` | ×2.5 | H1 |
| `List_UpgradeBrakes.GameFrictionScaleBraking` | ×1.3 | B1 |
| `List_UpgradeCarBodyWeight.BlockDimX`, `BlockDimZ` | ×1.2 (≈ +44% yaw inertia) | Y1 lazy yaw (test separately; collision use unknown) |
| `List_UpgradeDrivetrainDifferential.RearLimitedSlipTorqueAccel` | → race-level value | L2 |
| `Data_Car.GameTorqueScale` | ×1.15 (optional) | L1 punch |

### 3.3 Tier 2: shared tire rows (more MW-like, needs cloning)

- **Flattened lateral curves (G2).** For each `List_TireFrictionCurve` used as a lateral curve,
  set `v[i] = max(v[i], peak)` for i past the peak.
- **Launch traction (L1).** `TorqueFreeLongFrictionScaleAccel0` 1.3 → 1.05 (below Accel1). This gives wheelspin at launch, then hook-up.
- **Cloning, so only tuned cars change.** Insert a copy of the compound with a new
  `TireCompoundID` (the 69000/69200 rows show the game tolerates extra IDs), copy its
  `List_TireFrictionMultiCurve` and `List_TireFrictionCurve` rows under new IDs, then point this
  car's `List_UpgradeTireCompound` rows (Ordinal = car) at the clone.
- **Cloning risks:** shop and upgrade UI lookups by compound ID, online/Drivatar consistency, and
  the garage save storing the compound part ID. Simpler alternative: **apply Tier 2 globally**, as
  a "MW world" toggle, and accept that every car changes, AI included.

### 3.4 What will and won't feel like MW

**Likely to feel MW-ish:**
- Higher, more forgiving grip, with no snap past the limit once the curves are flattened.
- Strong high-speed stability from the downforce floor and the rear-biased grip.
- Narrow lock at speed.
- A handbrake that swings the tail.
- Fast countersteer.
- More launch wheelspin.

**Can't be replicated without live sim hooks:**
- **Lazy yaw (YAW_SPEED 0.25):** only partly, through inertia.
- **The stability boost that the handbrake turns off.**
- **The drift state machine:** yaw damping and countersteer-driven rear friction. Without it,
  FH2 drifts stay "Forza drifts", just easier to hold.
- **Grip/slide hysteresis.**
- **Linear downforce.**
- **The clutch snap, burnout fishtail and perfect launch.**
- **Off-throttle drag ×2.**
- **No-bounce walls and ×15 contact inertia.**
- **Auto-brake.**

Approach for hooks if ever wanted:
- Ports of MW handling to other sims (for example a community Assetto Corsa port) did it by replacing the tire model, so the real thing means owning the tire force function.
- In FH2 that means finding the per-wheel friction evaluation in the sim (the physics builder is at
  game+0x1056210 per swap.md) and the live sim object. Only then could YAW_SPEED-style or
  drift-state terms be added.
- This is high effort and unknown feasibility.

### 3.5 Risks

- **Shared rows.**
  - Tire compound and curve rows are shared by hundreds of cars, AI included.
  - Engine and camshaft rows are shared per stock engine (LiveTune already warns about this).
  - Check `List_UpgradeDrivetrainDifferential` sharing per DrivetrainID before editing it.
  - `AutoSteerOverrides` has only 3 rows, all shared.
- **Negative downforce.** Multiplying lift makes it worse. Use floors.
- **Hidden clamps.** The `Lat/Long*ScalarMax/Clamp` columns may cap grip gains. If ×1.5 friction
  does nothing, look there.
- **Sane ranges.**
  - Keep friction scales ≤ about 1.8×.
  - Keep downforce ≤ about 600, the DB max.
  - Keep BlockDim ≤ ×1.3.
  - Keep BiasHandbrake ≤ about 5.
  - Huge grip with stock springs can cause roll-over, bouncing or tire-load spikes. If that
    happens, stiffen `List_AntiSwayPhysics.DefSwaybarStiffness` and
    `List_SpringDamperPhysics.DefSpringRate` (with Min/Max clamps, since the tuning menu range
    comes from Min/Max).
- **PI and class drift.**
  - Car (re)preparation recomputes PI and stats from the edited DB.
  - Per swap.md, `0x2cfa50` writes ClassID, PI, CurbWeight and more into the saved garage row
    when the car is row-backed. MW-tuned cars may jump class and get locked out of events, and
    that is persisted in the save.
  - Decide whether the preset is "free roam / for fun" or must restore stock before saving.
- **Semantics.** Every *(semantics unknown)* lever needs an in-game A/B check before it goes into a
  preset. Check by feel (or a memory dump with your own tools), one lever at a time.
- **Tuning menu interaction.** Player tune sliders (downforce, brake balance, diff) sit on top of
  the DB defaults, and a saved tune file may override Def* values.

### 3.6 Suggested test order (each alone, on one RWD car, e.g. E46 M3 Id 383)

1. ChassisStiffness friction ×1.5 (Tier 1 grip).
2. FixListingRearFricScale 1.35 → 2.0 (does anything change at speed?).
3. SteerSpeedSensitiveMinMaxAngle 5 → 3 (pad, normal steering).
4. BiasHandbrake ×3.
5. Downforce floor.
6. BlockDimX/Z ×1.2.
7. A flattened-curve clone of one compound (Tier 2).
