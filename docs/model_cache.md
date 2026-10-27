# Why same-car reloads keep old physics: the CarCache (2026-10-27)

Static research on `forza_durango_release_final.exe` (FH2 1.0.0.64). All addresses are RVAs (game+X).
Class names come from RTTI. Nothing was run in-game. I used capstone with throwaway helper scripts
in my scratchpad.

> Release note: `physics_trace.c`, mentioned below, was a development-only diagnostic (an entry
> hook that logged calls to the physics builder). It is not part of the Enhanced Tuning release.
> The fix this report led to is `src/car_rebuild.c` (§4.1) together with `src/car_reprepare.c`.

Confidence: **high** = read directly in the code; **medium** = inferred from code plus naming or data
flow; **low** = guess.

## TL;DR

1. **The `346e1e` stack is not the car you drive.** It's the **Tuning screen's "Benchmark" preview**:
   `0xea0ba0` (Tuning page, `"Tuning"` at 0xe912cf) → `0xbc43c0` (`"Benchmark"`) → `0x340530` →
   `0x346e50`. It builds a throw-away 0x38f0 car definition from the DB and a throw-away
   `CCarDynamics` every time, which is why it always sees fresh DB values. (high)
2. **The driven car's physics is built by `Car::Init` → `0xf67270` → `0xf670d0` → builder 0x1056210,
   and the builder returns to `0xf671b7`.** `physics_trace.c` labels exactly that caller as
   `AI_STREAMING_CALLER` and drops it. Player and AI cars both go through this path. (high)
3. **Same-car reloads skip that path entirely because of `EntityModel::CarCacheModule`.** Unloading
   the player car doesn't destroy the `Car`. It stays loaded and hidden (`"car_cache_hidden"`) in the
   one-slot **"Player LOD0"** cache group. The next `LoadPlayerCar` request matches the hidden car by
   **model id + the 0xB4-byte upgrade block + a few flags** and gets the **same `Car` object back**,
   with no Init and no physics build. (high)
4. **A swap works because the model id doesn't match.** The single slot is evicted, `Car` is destroyed,
   and the new car runs a full Init, which re-reads the DB. Swapping back does the same. (high)
5. **The best fix is to call the game's own in-place rebuild: `Car` vtable slot 0x48 = `0xf75610`.**
   It rebuilds the 0x38f0 definition from the DB with the builder, then calls
   `CCarDynamics::ReinitFromDefinition` (interface vf 0x238) on the **live** dynamics. No respawn is
   needed. (Signature and code path: high. Live behaviour and native callers: not verified.)
6. Alternatives: evict the hidden cached car between unload and load (§4.2), or poke the live
   `CCarDynamics` fields (§5: GameTorqueScale at `CCarDynamics+0xfec`, redline at `+0x568`).
7. Side findings:
   * The builder **clamps `GameTorqueScale` and `GameDragScale` to [0.5, 1.5]**. (high)
   * `Data_Car.SimRedlineAngVel` is **not read by the physics builder**. The only code that
     references the string is 0x2a4040, which reads garage/stat rows. The physics redline comes from
     the engine `RedlineRPM` (§5). (medium-high)

---

## 1. What each path reads

### 1a. The `346e1e` stack (Tuning preview) — high

```
0xe94e70 / 0xe93ec0  Tuning page (jmp / call 0xea0ba0)
0xea0ba0  car = PM->vf48->vf90 ; spec = 0xf60f30(car) ; tune = [this+0xe08]+0x1a8
0xbc43c0  "Benchmark" ...  -> 0x340530(obj+0xd8, spec, tune)
0x340530  -> 0x346e50
0x346e50  CCarInGarage = 0x2dca30([player]->vf d8)  (garage+0x50), spec copied from it (0x21ded0)
          def = new 0x38f0 (ctor 0x12a9a10); 0x346c40 -> builder(def, &[[0x34aba60]+0x40], spec,...)
          dyn = [0x367bab8](CPhysics)->vf18(def)   -> fresh 0x52b0 CCarDynamics (0x12bfe40)
```

This reads the DB directly. The garage `CCarInGarage` only supplies ids: the car id and the upgrades.
None of this reaches the car in the world.

### 1b. The driven car — high

```
LoadPlayerCar 0x8ae8e0
  returns if player->vf90 (car) != 0
  spec = [0x34aba60 obj via 0x13f0ec0]->vf40(out, {player, choices}, ...)   // from the choices
  0x4814d0(module, &out, spec, 2, 0, "LoadPlayerCar")
    -> 0x4813f0 -> 0x481380: group = module+0x50 + (spec+0x488 != 0)*0xa0
    -> 0x481230: new CarCacheRequest (0x5d0, vtbl 0x2c81240, ctor 0x461e10), push to group+0x78
CarCacheModule::Update 0x4748f0 (per frame, both groups):
  0x47b940 -> 0x47ba20  TRY REUSE a hidden cached car  (cache hit -> nothing rebuilt)
  0x474120              assign remaining requests to EMPTY slots -> 0x47b2e0 -> new Car
  0x480a60
  0x487430              evict hidden cached cars while (requests + used) > keep
Cache miss: Car vtbl slot 0x38 Init (Car 0xf65ad0; ForzaCar 0x294810 wraps it)
  -> 0xf67270(car, ...)  : def is embedded at car+0x1200 (0x38f0 bytes)
     -> 0xf670d0 -> builder 0x1056210(car+0x1200, &[[0x354f4f8]+0x40], {car+0x118 id, car+0x120 upgrades},...)
     -> [car+0x3a8] (CPhysicsWorld) vf80 = 0x12bfee0: new CCarDynamics(def) -> [car+0x4c08]
```

The builder runs `SELECT * FROM Data_Car WHERE Id=` and reads `GameDragScale` (→ def+0x158) and
`GameTorqueScale` (→ def+0x15c), both clamped to [0.5, 1.5]. It also reads the engine row
(0x105ccf0 → 0x105d690: TorqueScale, RedlineRPM, StallRPM, StartRPM, ...) into def+0x534.., with
**RedlineRPM at def+0x580**. It also reads the drivetrain, the car body, friction scales, MassDiff
and so on.

So the driven car **is** built from the DB, but only when `Car::Init` runs. That only happens on a
cache miss. The `CCarInGarage` at garage+0x50 and the choices matter only because they supply the
**spec** (model id + upgrades + tune). They hold no physics values.

## 2. The cache

### Layout — high

`EntityModel::CarCacheModule` (RTTI 0x301d918). It exposes reflected methods `Flush(CacheGroups)` =
**0x474660** and `FlushAll()` = **0x474680** (registered at 0x4634b0).

| where | what |
|---|---|
| module+0x50 | group 0 **"Player LOD0"**: 1 slot, keep 1 (0x47a570 → 0x47a360(..., 1, 1)) |
| module+0xf0 | group 1 **"Non-LOD0"**: N slots (config, default 11, max 12) |
| group+0x30 | name (std::string) |
| group+0x60 / +0x68 | slot vector, **0x590 bytes per slot** |
| group+0x78 / +0x80 | queued `CarCacheRequest*` vector |
| group+0x90 | free load slots (init = slot count; −1 in 0x47b2e0, +1 when a load finishes) |
| group+0x94 | how many loaded cars to keep cached while hidden |
| slot+0x000 | copy of the car spec (0x550 bytes) |
| slot+0x550 | owner key (= request+0x568) |
| slot+0x558 | the request currently using the slot (0 = hidden/cached) |
| slot+0x560 | the loaded entity (`[slot+0x560]+0x28` = `Car*`) |
| slot+0x568 | pending load job |
| request+0x10 | spec (0x550 bytes) |
| request+0x568 | owner key |
| request+0x598 | state (Queued/Ready/Released; 1 = Ready, 3 = Released) |
| request+0x5a0 | `Car*` |

The player's spec has spec+0x488 == 0, so it goes to group 0. That last step is inferred from
0x481380 plus the group names. (medium-high)

### Key — high

The match is `0x458730`, using `0x46ff90(carSpec = car+0x110, requestSpec, compareUpgrades)`. A
cached slot matches when **all** of these hold:

* slot has no request, no load job, and has an entity;
* `slot+0x550 == request+0x568` (same owner);
* `spec+0x08` equal → **model id** (`car+0x118`);
* `spec+0x2ed == 0` on **both** sides (`car+0x3fd`, a "don't reuse" flag);
* `spec+0x488` equal (`car+0x598`, human/AI driver type);
* `spec+0x2e5` equal (`car+0x3f5`);
* if `request+0x562 == 0` (it is 0 for `LoadPlayerCar`): `memcmp(spec+0x10, spec+0x10, 0xB4) == 0`
  → **the upgrade block** (`car+0x120`).

The DB contents are not part of the key, and neither is the tune. So editing Data_Car and reloading
the same car always hits the cache.

### What evicts it — high

* **Release** (unload) is 0x480e60. It detaches the car, hides it (0x479d10) and clears
  `slot+0x558`. It destroys the slot (0x487280) only if `usedSlots > keep`. For "Player LOD0" that
  is `1 > 1`, false, so the car stays cached.
* **Demand eviction** is 0x487430. When a queued request didn't match (for example a different model
  id) and there's no empty slot, it destroys hidden slots. That is the swap case.
* **`Flush(group)`** is 0x474660 → 0x474590. Every slot is destroyed through 0x487280. Active
  requests are first released (0x480e60), reset to state 0 and re-queued, so they reload.
  **`FlushAll`** (0x474680) does the same for both groups.

## 3. Answers

**Q1.** The driven car's physics comes from the DB (builder at Car::Init), but Init only runs on a
CarCache miss. The 346e1e stack is the Tuning-screen benchmark sim, not the driven car. The garage
`CCarInGarage` and the choices only supply the spec (key). (high)

**Q2.** The cache is `CarCacheModule`, "Player LOD0" group = module+0x50, with one slot. The key is
model id + 0xB4 upgrade block + flags + owner (§2). A swap evicts it through 0x487430 because the
model id differs. To force a re-read, see §4.

**Q3.** Yes. Each car has its own `CCarDynamics`: the object is at `[car+0x4c08] - 0x3a0`, and
`[car+0x4c08]` is its interface pointer. Offsets are in §5.

## 4. What a plugin can do (recommended order)

### 4.1 Recommended: `Car::ApplyUpgrades` (Car vtable slot 0x48) in place, no respawn

`void 0xf75610(Car* car, const uint8_t upgrades[0xB4], bool resetTuning)` — prologue
`48 89 54 24 10 55 53 56 57 41 54 41 55 41 56 41`. `Car` (vtbl 0x2e6b0a0) and `ForzaCar`
(vtbl 0x2c5cb80) both have 0xf75610 at slot 0x48. (high)

What it does (read from the code; high):

1. Builds a new 0x38f0 def on the stack (0x12a9a10). Uses car id `[car+0x118]` and the passed
   upgrades. The tune comes from `car+0x1d4` when `resetTuning == 0`, or from defaults
   (0x283150, 9 floats = −1) when it is 1.
2. `0xf670d0(car, &def, &[[0x354f4f8]+0x40], spec, ...)` → **builder 0x1056210** re-reads
   Data_Car and the engine/upgrade tables. It uses the same DB connection as a fresh Init.
3. `[car+0x4c08]->vf238(&def, 3)` → 0x12da500 → **`CCarDynamics::ReinitFromDefinition` 0x12da510**.
   This re-runs the engine (0x12e10e0 → 0x12e0a50), torque (0x12e07e0) and other setup on the live
   dynamics.
4. Copies the new def into `car+0x1200` and the upgrades into `car+0x120`. Pushes them to the model
   (`[car+0x4d38]` vf b8/f8/100/108). Reloads the tuning setup (0x104b9c0(car+0x4af0, ...)). Re-reads
   the shift system, tires and brakes.

Suggested call (game thread, car spawned, game paused or on the Tuning screen):

```c
char *car = vehicle_update_car;                       // car+0x118 model id, car+0x390 CPlayer
uint8_t up[0xB4]; memcpy(up, car + 0x120, 0xB4);      // current upgrades, unchanged
void (*apply)(void *, const void *, uint8_t) = (void *)(game + 0xf75610);
// check: *(void**)(*(char**)car + 0x48) == game+0xf75610, and the prologue bytes above
apply(car, up, 0);                                    // 0 = keep the current tune
```

Not verified:

* I found no native caller. It is presumably the "upgrades changed while spawned" path (upgrade
  shop or Tuning apply).
* Whether it's safe mid-race.
* Whether every downstream consumer (HUD tach, audio, AI) picks up the new redline.
* It re-reads the Upgrade tables, so any LiveTune edits to List_Upgrade* apply as well (expected).

Test: hook the builder and confirm it fires from `0xf75a81` (the return address after the
0xf670d0 call). Read `CCarDynamics+0xfec` before and after (§5).

### 4.2 Alternative: evict the cached player car, then do EO's normal reload

Between `unload_player_car` and `load_player_car`, the player car sits hidden in group 0. Destroy it
the way the game does for a swap:

```c
char *group = module + 0x50;                          // "Player LOD0"
for (char *s = *(char**)(group+0x60); s != *(char**)(group+0x68); s += 0x590)
    if (!*(void**)(s+0x558) && *(void**)(s+0x560) && !*(void**)(s+0x568))
        ((void(*)(void*, void*))(game + 0x487280))(group, s);   // 40 55 56 57 48 81 ec c0 05 00 00
```

The next load is then a miss → full `Car::Init` → fresh DB read. (Logic: high. Untested.)

Getting `module`:

* There's no static global. I couldn't find one quickly.
* Capture `rcx` in a hook on `CarCacheModule::Update` **0x4748f0** (`48 89 5c 24 08 48 89 6c 24 10
  48 89 74 24 20 57`), which runs every frame.
* Or hook 0x4813f0's entry (rcx = module, on each car request).

`Flush(module, 0)` = 0x474660 (`48 63 c2 48 83 c1 50 ...`) also works at that point, because no
active player request exists then. **Don't call Flush while the car is spawned.** It releases and
re-queues the active request, and destroys the `Car` the player still points at.

Cheaper but hackier: after unload, set `car+0x3fd` (spec+0x2ed) = 1 on the hidden car. The match
then fails and 0x487430 evicts it on the next load. Don't do this while the car is live: ForzaCar
vf 0x80 (0x2a9400) passes this byte to the render model every update, and 0xf68c60 treats it as a
livery-override flag. (medium)

### 4.3 Alternative: poke the live dynamics (no DB re-read)

The fields are in §5. It's fast and touches nothing else, but only covers the fields you write. Some
values are copied to several places (for example `0x12e0a50` derives several engine fields from the
redline).

## 5. Per-instance fields (from the vehicle-update `car`)

```
car+0x118                     model id (= spec+0x08)          high
car+0x110                     spec (0x550); car+0x120 = 0xB4 upgrade block; car+0x1d4 tune   high
car+0x1200                    embedded 0x38f0 physics def (builder output)                   high
   def+0x158 / +0x15c         GameDragScale / GameTorqueScale (clamped 0.5..1.5)             high
   def+0x534..                engine block; def+0x57c TorqueScale(engine row), def+0x580 RedlineRPM   high
car+0x3a8                     CPhysicsWorld*                                                 high
car+0x4c08                    CCarDynamics interface ptr (= dyn + 0x3a0)                     high
dyn = [car+0x4c08] - 0x3a0    CCarDynamics (0x52b0 bytes, vtbl 0x2eb5d68)
   dyn+0xf38                  torque/drive block; filled by 0x12e07e0(dyn, dyn+0xf38, def)
   dyn+0xfec                  = def+0x15c GameTorqueScale; read every tick by 0x12b519c,
                              0x12b5820 (x2), 0x12b6417 (vmulss [rbx+0xfec])            high (that it is read); medium (that it is the only copy)
   dyn+0x568                  = def+0x580 * 2π/60  (redline, rad/s), 0x12e0c39          high (write); low (which other fields derive from it)
   dyn+0x55c/560/564          derived from def+0x534/538/53c/540 (engine torque/inertia?) medium
```

As offsets from the interface pointer `p = [car+0x4c08]`: torque scale is at `p+0xc4c` and redline
is at `p+0x1c8`. Check these against a memory snapshot of the car (the development build had an F9 snapshot for this; it is not in the release) before writing them.

## 6. How to confirm in-game (cheap)

1. In `physics_trace.c`, **stop dropping `0xf671b7`**. Log it when `rcx == car+0x1200` for the
   player car. Then a same-car reload should show *no* builder call (cache hit), and a swap should
   show one from `0xf671b7`.
2. Hook `0x47ba20` and log its return value (true = cache hit). It should be true on a same-car
   reload.
3. Try §4.1 from the pause menu. The builder should fire from `0xf75a81`, and `dyn+0xfec` should
   change to the new GameTorqueScale (clamped).

## Entry bytes

| RVA | what | first bytes |
|---|---|---|
| 0xf75610 | Car vf 0x48 ApplyUpgrades(car, up[0xB4], resetTune) | `48 89 54 24 10 55 53 56 57 41 54 41 55 41 56 41` |
| 0x12da510 | CCarDynamics::ReinitFromDefinition(dyn, def, flags) (iface thunk 0x12da500 = `add rcx,-0x3a0; jmp`) | `48 8b c4 48 89 58 08 48 89 68 10 48 89 70 18 57` |
| 0x487280 | CarCacheGroup::DestroySlot(group, slot) | `40 55 56 57 48 81 ec c0 05 00 00 48 c7 44 24 60` |
| 0x4748f0 | CarCacheModule::Update(module) | `48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 20 57` |
| 0x474660 | CarCacheModule::Flush(module, group) | `48 63 c2 48 83 c1 50 48 8d 14 80 48 c1 e2 05 48` |
| 0x47ba20 | TryReuseCachedCar(group, &request) → bool | `4c 8b dc 49 89 5b 18 55 56 57 48 83 ec 60 48 8b` |
| 0x480e60 | ReleaseRequest(group, &request) | `40 56 57 48 83 ec 28 48 8b f9 48 8b 0a 48 8b f2` |
| 0xf65ad0 | Car::Init (vf 0x38) | `48 8b c4 44 88 40 18 48 89 50 10 55 56 57 41 54` |
| 0xf67270 | Car physics load (calls 0xf670d0) | `48 8b c4 44 88 48 20 48 89 50 10 55 56 57 41 54` |
| 0xf670d0 | wrapper → builder (returns to 0xf671b7) | `48 8b c4 4c 89 40 18 56 57 41 56 48 81 ec f0 00` |
