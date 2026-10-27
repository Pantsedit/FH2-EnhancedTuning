# Car swap / re-prepare: how "Get in Car" works natively (2026-10-27)

Static research on `forza_durango_release_final.exe`. All addresses are RVAs (game+X).
Class names come from the EXE's RTTI. I disassembled with capstone using small throwaway
helper scripts (similar to `tools\disasm.py`). Nothing was run in-game.

Confidence: **high** = read directly in code; **medium** = inferred from code plus naming
or data flow; **low** = guess.

## TL;DR

* The "setup record" `car->[0x390]->[0x330]->[0x50]` is
  **`CPlayer -> CGarage -> m_careerCar`**, a cached, refcounted `CCarInGarage`
  (0xAB8 bytes, vtable 0x2c5f818). Of all garage cars, only the current career car is
  cached. A swap is what drops and rebuilds that cache. (high)
* Confirming a car on the car-select page runs two calls (high):
  1. `CGarage::SetCareerCar(garage, garageRowId, &extra)` = **0x2de4e0**
  2. `ForzaLocalPlayerChoices::SetGarageCar(choices, garage, garageRowId)` = vtable slot
     **0x1a8**, implemented at **0x2a0c30**

  The argument is the **garage row Id** (`NewProfile_Career_Garage.Id`, read from the list
  item's `"Id"` field), not a Data_Car id.
* `SetCareerCar` has **no "same car" early-out**: it always releases `garage+0x50` and
  rebuilds the `CCarInGarage` (→ 0x2dc8f0 → 0x2dac40(force=1) → 0x2be790 → 0x2f5120 →
  0x2cfa50). Calling it with the **current** id should re-prepare the same car without a
  second car (medium-high; this needs an in-game test, see the end).
* The stateflow already has the "re-copy the garage car into the player's choices" step:
  `global.select_in_current_garage_car` (behavior `GetInCurrentGarageCar`, 0x88d4a0) does
  exactly call 2 for the current career car.

## 1. What the car-select page calls on confirm

The behavior name `UIPageCarSelectGaragePauseMenu` is only registered (0x93fd0 →
descriptor 0x34ad6a0 → 0x18a2340). The page code lives in the RTTI classes
`CarSelectGarage` (vtbl 0x2d6cb90) and `CarSelectCareerGarage` (vtbl 0x2dd1f68; it
derives from the same base, and slot 0x5a8 is overridden). I did not pin down which of
the two the pause-menu page instantiates (medium). Both end in the same pair of calls.

**CarSelectGarage path** (popup: Get in Car / CarFlow options …):

* vtbl slot 0x5a8 = 0xccf600 "item confirmed": builds the popup. The closure vtable is
  0x2ddee70, and its thunk 0xcdade0 jumps to 0xcc34e0.
* 0xcc34e0 → `0xccc370(page, optionIndex = [popupItem+0x68], listItem)`. Option 0 (Get in Car):

```
ccc638  lea rcx,[rbp-0x21] ; call 0x215ec0        ; construct empty "extra" (tune/livery) arg
ccc64d  call 0xced510                              ; garage = localPlayer->vf[0xd8]
ccc655  mov r8,rbx ; mov edx,r15d                  ; r15d = item->vf[0x38](item,"Id")  (garage row Id)
ccc65b  call 0x2de4e0                              ; CGarage::SetCareerCar(garage, id, &extra)
ccc660  call 0xced1e0                              ; choices = localPlayer->vf[0x100]
ccc66b  call 0xced510                              ; garage again
ccc679  call [rdi+0x1a8]                           ; choices->SetGarageCar(garage, id)  (0x2a0c30)
ccc67f  call 0xced1b0 ; xor edx,edx ; call 0x2f3820; career bookkeeping on [player+0x328] (not needed)
ccc68e  ... fire page transition (car_selected)
```

**CarSelectCareerGarage path**, slot 0x5a8 = 0xccf460: reads `"Id"` and `"IsCurrent"`
from the item, then does the same thing inline:

```
ccf519  lea rcx,[rsp+28h]; call InitializeCriticalSectionAndSpinCount ...  ; inline extra ctor
ccf548  call 0xced510             ; garage
ccf557  call 0x2de4e0             ; SetCareerCar(garage, id, &extra)
ccf55c  call 0xced1e0             ; choices
ccf575  call [rdi+0x1a8]          ; choices->SetGarageCar(garage, id)
        then transition (different one if IsCurrent)
```

In ui_screens.xml the pause-menu state maps `car_selected` straight to `.back`, so the
in-world respawn after "Get in Car" happens somewhere native. **I did not find where.**
Our sequence below does not depend on it, because it uses an explicit unload/load.

## 2. Signatures and how to get the objects

### Object getters (high)

| what | how | evidence |
|---|---|---|
| PlayerManager | `*(void**)(game+0x34b3e18)` | 0x327270 `mov rax,[0x34b3e18]; ret` |
| local `CPlayer*` | `PM->vtbl[0x48/8](PM)` (UI path; 0xb2bca0 AddRefs it). Also `car->[0x390]`. | 0xb2bca0, 0x8ae8e0 |
| `CGarage*` | `CPlayer` vf 0xd8 = `[player+0x330]` | CPlayer vtbl 0x2c62bf0 slot 0xd8 = 0x323b50 |
| player choices | `CPlayer` vf 0x100 = `[player+0x348] + 0xae8` (`TRefCountedObject<ForzaLocalPlayerChoices>`, vtbl 0x2cf39c0) | slot 0x100 = 0x323b60 |
| player's car | `CPlayer` vf 0x90 = `[player+0x320]` (out-param form) | slot 0x90 = 0x323b40 |
| current career garage Id | `uint32 0x2dcab0(CGarage*)` reads profile setting `"Main/CareerCar"` | 0x2dcab0 |
| cached career car | `[garage+0x50]` (`CCarInGarage*`, may be null); `0x2dca30(garage, TRef* out)` returns it AddRef'd, building it if null | 0x2dc8f0 |

`CGarage` layout used here: +0x08 profile settings handle, +0x10 `std::string` table
name (the profile's `..._Career_Garage`), +0x50 cached career `CCarInGarage`.

### CCarInGarage (0x2be790 constructor) — high

`CCarInGarage* 0x2be790(CCarInGarage* this /*0xAB8, from game alloc 0x3229c0*/, CGarage* garage, uint32 garageRowId, Extra* extraByValue)`

* r9 is a by-value struct passed by hidden pointer, and **the callee destroys it**
  (0x2bf3b5..0x2bf3e8: string reset, `[+0x28]` Release, `DeleteCriticalSection`).
* Runs `SELECT * FROM <garage> WHERE Id=<row>` and fills: CarId → `+0x40`, Guid,
  ClassID, Flags, MakeID/EnginePlacementID (from Data_Car), stats, the 0x2B upgrade part
  ids, WheelStyle, TuneFileName, Versioned*Id.
* Calls `0x2f5120(this, &upgrades, 0)` while `this+0x2bc` (row id) is still 0, so no
  DB writes. Then sets `+0x2bc = row`. Then loads the tune: if `extra[+0x28]` is null,
  via `0x2e8b00(garage, out, row)`. Then `0x2cd960(this, tune)`.
* Layout: `+0x40` CarId, `+0x48` 0xB4-byte upgrade block (+0x4c WheelStyle, +0x50..
  0x2B part ids), `+0x260..0x2b8` PI/ratings/peak power/weight, `+0x2f4`/`+0x6cc`
  curves (count at +0xaa4), `+0x2bc` garage row Id, `+0xaa8` owning CGarage. `+0x40..`
  is also the car spec that `SetGarageCar` copies (0x21ded0 returns `this+0x40`).
* **Don't call it directly.** Use `GetGarageCar`/`SetCareerCar`, which handle
  allocation and refcounting.

Wrappers:

* `0x2dadd0(CGarage*, TRef<CCarInGarage>* out, uint32 row, bool forceNew)`: if
  `!forceNew && row == Main/CareerCar`, it returns the **cached** `[garage+0x50]`
  (0x2dc8f0). Otherwise it allocates a **fresh, uncached** object.
* `0x2dc8f0(CGarage*, TRef* out, Extra* byValue)`: if `[garage+0x50]` is null, builds
  it with 0x2dac40(force=1). Returns it AddRef'd.

### 0x2f5120 = CCarInGarage::SetUpgrades — high

`void 0x2f5120(CCarInGarage* this, const Upgrades* src /*0xB4 bytes*/, bool r8)`

1. Derives engine/aspiration fields (+0x2c8..+0x2dc) from src, then copies src to
   `this+0x48`. Computes AspirationTypeId (+0x288).
2. **If `this+0x2bc != 0` it writes save data**:
   `UPDATE <garage> SET WheelStyle=…, <43 part columns>=…, AspirationTypeId=… WHERE Id=<row>`.
3. Calls `0x2cfa50(this)` (stats rebuild). If r8 is set, also `0x2ffa80(this, 0)`.

`0x2cfa50(this)` runs the loader `0x2d0160(stackOut, [this+0x40], this+0x48)`. The
loader builds a temporary 0x38f0 sim with the physics builder 0x1056210 and computes PI.
It copies the results from `[0x365d700]` into `this+0x260…`, plus the curves. **If
`+0x2bc != 0` it writes**
`UPDATE <garage> SET ClassID, PerformanceIndex, SpeedRating, …, SimPeakPower, …, CurbWeight, WeightDistribution WHERE Id=<row>`.
That would write PI/weight computed from our edited Data_Car into the saved garage row.
**So do not call 0x2f5120 or 0x2cfa50 on the live (row-backed) career car.**

### 0x2de4e0 = CGarage::SetCareerCar — high

`void 0x2de4e0(CGarage* garage, uint32 garageRowId, Extra* extraByValue /*consumed*/)`

```
2de519  "SELECT 1 FROM %s WHERE Id = %u"   ; row must exist (owned car), else nothing happens
2de57b  0x2f7940(garage->settings, "Main/CareerCar", id)   ; profile setting (saved)
2de596  release [garage+0x50]; [garage+0x50] = 0            ; <-- drops the cache
2de5c4  0x2dc8f0(garage, &out, extra)                       ; rebuild via 0x2dac40(force=1) -> 0x2be790
2de5cf  0x2ffda0(garage, id)                                ; re-get car, rebuild ProfileN_View_Upgrade* SQL views
2de673  0x726210([0x34c0f40], 0x34bbcc0, evt{carId})        ; posts a "career car changed"-type event
```

There is no check for "id already current".

`Extra` (constructor **0x215ec0**, ≥0x60 bytes): +0x00 CRITICAL_SECTION (spin 0x600),
+0x28 refptr (tune), +0x30 `std::wstring` (SSO, cap at +0x48), +0x50 qword, +0x5c byte.
An empty one means "load the garage's own tune".

### 0x2a0c30 = ForzaLocalPlayerChoices::SetGarageCar (vf 0x1a8) — high

`void (choices, CGarage* garage, uint32 row)`:

* `0x292380` → `0x2dadd0(garage, &car, row, 0)`. For the career id this returns the
  cached object.
* Copies the car's spec (`car+0x40`) into `choices+8` through vf 0x198 (0xfabf40 →
  0x28a090). Copies stats (+0x230..), sets `choices+0x260 = row`, and sets the
  `choices+0x264` "changed" byte only if the row id changed.
* `GetInCurrentGarageCar` (0x88d4a0, vtbl slot 0x20 of `States::GetInCurrentGarageCar`)
  does exactly `choices->SetGarageCar(garage, 0x2dcab0(garage))` (+ optional
  clear_forced_pi).

`LoadPlayerCar` (0x8ae8e0) returns early if `player->vf90` (car) is set. Otherwise it
builds the spawn spec from the choices: `[[0x34aba60]+0x50]->vf[0x40](out, {player,
choices}, player+0x40)`, then `0x4814d0(…, "LoadPlayerCar")`. (medium on the details)

## 3. Lighter path / same car / unowned car

**Why same-car reload is stale.** The unload/load sequence never touches
`garage+0x50`, and it never re-runs `SetGarageCar`. So the spawn uses the old
`CCarInGarage` and the old copied spec. A swap A→B→A runs SetCareerCar twice, which
destroys A's cached object and rebuilds it from the DB. (The "same record address" seen
on swap-back is almost certainly the allocator reusing the freed 0xAB8 block.) (medium-high)

**Proposed lighter path (no second car)** (medium-high, untested). Call
`SetCareerCar(garage, currentId, &emptyExtra)` and then `choices->SetGarageCar(garage,
currentId)`. That is exactly what a confirm on the already-current car does natively,
and it rebuilds the cache. Then do one unload/load. Side effects:

* "Main/CareerCar" is rewritten with the same value.
* The upgrade SQL views are recreated (DDL on the shared DB).
* One car-changed event is posted.
* No garage-row UPDATE: the constructor runs 0x2f5120 with row 0. (high)

**Open risk:** the stale data could instead live in something the spawn caches per car
id, such as loaded car assets or the sim built at spawn. The spawn path does call the
builder: `0xf67270` → `0xf670d0` → `0x1056210` with `[car+0x118]` and the DB
`[0x354f4f8]+0x40`. If the builder doesn't run on a plain reload, or reads a different
connection than the one EO gives plugins, then only a real A→B→A swap (with B spawned)
helps. **Test:** hook 0x1056210 during a same-car EO reload. If it fires and reads the
edited row, the light path is enough.

**Not recommended:** calling 0x2f5120/0x2cfa50 on the live car. They rebuild stats, but
they write PI/CurbWeight/parts to the saved garage row, because `+0x2bc != 0`.

**Car the player doesn't own.** `SetCareerCar` and `GetGarageCar` need a garage row
(`SELECT 1 …`), so they can't take one. `ForzaLocalPlayerChoices` vf **0x1c8 =
0xfab530** `(choices, uint32 dataCarId/ModelId, const Upgrades* 0xB4)` sets a raw
Data_Car car (`SELECT * FROM Data_Car WHERE ModelId=`, `choices+0x260 = -1`). Rentals and
forced cars use this route (`IsInGarageCar` becomes false; `exit_rental_car` restores via
`select_in_current_garage_car`). (medium) It does **not** rebuild `garage+0x50`, so on
its own it doesn't help the stale-cache problem. It's only useful as the "B" car if the
asset-cache theory turns out true. A stock upgrade block is initialised by 0x1042d10
(medium).

## 4. Proposed call sequence

Do everything on the game thread (FH2PluginVehicleUpdate). Do it while paused or inside
the reload, never mid-frame from another thread. All callees take the DB and internal
critical sections.

```c
// 0) Finalize/reset any of our own prepared statements on EO's connection (the views DDL can
//    invalidate them -> SQLITE_SCHEMA / busy).
void *pm      = *(void **)(game + 0x34b3e18);
void *player  = vcall(pm, 0x48);                       // CPlayer* (or car->[0x390])
void *garage  = *(void **)((char *)player + 0x330);    // CPlayer vf 0xd8
char *holder  = *(char **)((char *)player + 0x348);    // CPlayer vf 0x100 (check non-null)
void *choices = holder + 0xae8;
uint32_t id   = ((uint32_t (*)(void *))(game + 0x2dcab0))(garage);   // Main/CareerCar row
// sanity: [garage+0x50] non-null and [[garage+0x50]+0x40] == the Data_Car id we edited

// 1) apply DB edits (Data_Car etc.)

// 2) rebuild the cached career car (same id)
_Alignas(16) unsigned char extra[0x80];
((void *(*)(void *))(game + 0x215ec0))(extra);                       // ctor
((void (*)(void *, uint32_t, void *))(game + 0x2de4e0))(garage, id, extra); // consumes extra: do NOT destroy

// 3) push the new spec into the player's choices (= GetInCurrentGarageCar)
(*(void (**)(void *, void *, uint32_t))(*(char **)choices + 0x1a8))(choices, garage, id);

// 4) one reload: store_car_position -> unload_player_car -> [select_in_current_garage_car]
//    -> load_player_car -> restore_car_position (EO's _fh2_car_store sequence / car_reload.xml).
```

**Fallback if the light path is still stale** (real swap). Pick another owned row B (the
garage table has the rows):

1. `SetCareerCar(B)` + `SetGarageCar(B)`, then reload (spawns B).
2. `SetCareerCar(A)` + `SetGarageCar(A)`, then reload (spawns A rebuilt).

This is the network.xml `exit_rental_car` pattern done twice. Two loading screens.

### Risks

* **Save data.**
  * The light path rewrites `Main/CareerCar` with an unchanged value. No garage-row writes.
  * The fallback leaves `Main/CareerCar = B` between the two steps. An autosave or crash
    there makes B the saved current car (annoying, not destructive).
  * Never call 0x2f5120/0x2cfa50 on a row-backed car (see §2).
  * The posted car-changed event (0x726210) may have listeners (UI, telemetry, possibly
    autosave). Not traced.
* **Threading.** Same thread as the UI/stateflow behaviors, which is where the game calls
  these. Calling SetCareerCar while the car is spawned is refcount-safe (the old object
  is Released, not freed, while others hold refs). Doing it between unload and load is
  cleaner.
* **UI state.** If the pause menu's car list is open, it holds items/refs to the old
  object. Do this outside the car-select page. `choices+0x264` stays 0 on the same-id
  path, so native "car changed" logic shouldn't fire.
* **Version fragility.** Check the prologue bytes of 0x2de4e0, 0x2a0c30, 0x215ec0 and
  0x2dcab0, and check that `CPlayer`'s vtable slots 0xd8/0x100 are 0x323b50/0x323b60, the
  same way the other LiveTune hooks do.

## Side finding (weight backlog)

`NewProfile_Career_Garage` stores `CurbWeight`, `WeightDistribution`, PI, peak
power/torque and every `Tuning_*` value per owned car. 0x2cfa50 recomputes these from
Data_Car and the upgrades, but writes them back only for row-backed cars. Physics weight
may come from upgrades plus Data_Car mass rather than `CurbWeight`. `MassDiff` appears in
0x2cfa50 via 0x2dccd0(this, 0x14/0x20). Worth tracing for `car+0x1360`.
