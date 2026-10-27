# Changelog

## 1.0.0 (first public release)

Released from the development build "LiveTune Probe" (0.1.0). Changes for release:

**Renamed**
- Mod folder `LiveTune Probe` → `Enhanced Tuning`; `mod.json` id `livetune-probe` → `enhanced-tuning`,
  name "Enhanced Tuning", version 1.0.0, new description.
- Plugin `LiveTuneProbe.dll` → `EnhancedTuning.dll`; entry source `livetune_probe.c` → `src/livetune.c`.
- Nothing depended on the old folder or DLL name: `LiveTune.ini`, `tunes.ini` and
  `FH2_ExtraOptions.ini` are all found relative to the plugin's own folder. The ini file names,
  the `[Car<Id>]` / `LiveTune*` keys in `tunes.ini` and the `LIVETUNE` log prefixes are unchanged,
  so an existing `tunes.ini` can be copied over as is.

**Removed (development-only diagnostics)**
- F9 car memory snapshot (wrote multi-megabyte dumps to `dumps\`), and the `tools/probe.py` /
  `tools/curvefind.py` readers for those dumps.
- F8 hardware data-breakpoint watcher (debug registers on every game thread, vectored exception
  handler, access report in the log).
- `physics_trace.c` (entry hooks on the physics builder and the car setup rebuild that logged
  callers) and `swap_trace.c` (entry hooks on the set-player-car path that logged arguments).
  These hooks are no longer installed at all.
- The "screen loaded" log line for every UI page passing through the resource hook.
- The per-car "vehicle update for car ... (setup record ...) on thread ..." log line.
- The slider getter log and the pointer/thread details in the slider setter log (the first 20
  slider changes are still logged as `LIVETUNE SLIDER: set <slider> <value> -> applied`).
- The startup line `LIVETUNE PROBE: ready. F9 snapshot; F8 watch (5 s).` is now
  `LIVETUNE: Enhanced Tuning 1.0.0 ready.`

**Added**
- `LiveTune.ini` now lists the existing `[Display] ScalePower` switch (default 1, unchanged).
- README, CREDITS, build script output in `release\`, research notes in `docs\` (audited and
  trimmed: personal paths and the Need for Speed source inventory and constants removed;
  unrelated map research left out).

No gameplay feature changed: the tab, the 15 sliders, the reload, in-place rebuild, stat-screen
horsepower and saved tunes work as in the development build.
