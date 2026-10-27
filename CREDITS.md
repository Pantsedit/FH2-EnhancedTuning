# Credits

## Author

- **Enhanced Tuning** by **Pantsedit** ([github.com/Pantsedit](https://github.com/Pantsedit)).
- **Claude** (Anthropic): AI assistance.

## Built on

- **Forza Horizon 2 Extra Options** by **Arthur (RokkuDayo)**:
  [github.com/RokkuDayo/ForzaHorizon2_ExtraOptions](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions).
  Enhanced Tuning is an Extra Options plugin. It relies on Extra Options' mod loader, its plugin
  API (`src/fh2_plugin.h` is Extra Options' plugin API header), the merged in-memory game database
  it hands to plugins, its UI resource hook (which Enhanced Tuning chains onto rather than
  replacing), and its Get in Car reload sequence. The way the sliders are added as native UI
  properties follows the approach Extra Options uses for its own settings pages.
- **Azimuth** by **Almost There Games**:
  [almost-there-games.itch.io/azimuth](https://almost-there-games.itch.io/azimuth).
  The translation layer that runs Forza Horizon 2 on Windows. Nothing from Azimuth is included.
- **Zig** ([ziglang.org](https://ziglang.org/)): `zig cc` 0.15.2 builds the plugin.
- **Capstone** ([capstone-engine.org](https://www.capstone-engine.org/)): the disassembler used by
  the optional research helpers in `tools/` (not included; install with pip).

## Inspiration

- **MW Handling** is inspired by community research into how **Need for Speed: Most Wanted
  (2005)** handles. The preset only rescales Forza Horizon 2's own database values toward
  "MW-like" targets (see `docs/mw_handling.md`). **No Need for Speed files, code or data are
  included or used.** Need for Speed and Need for Speed: Most Wanted are trademarks of Electronic
  Arts Inc.

## Forza Horizon 2

**Forza Horizon 2** is developed by **Playground Games** with **Turn 10 Studios** and published by
**Microsoft** (Xbox Game Studios). Forza, Forza Horizon and Xbox are trademarks of Microsoft
Corporation; all game content belongs to its owners.

**This project is not affiliated with, endorsed by or supported by Playground Games, Turn 10
Studios, Microsoft, Electronic Arts, Almost There Games or the Extra Options author.** It contains
**no game files**: no executables, UI files, databases, string tables, textures, audio or other
assets from Forza Horizon 2 or any other game. You need your own copy of the game. Game addresses
(RVAs), function and style names, and database column names appear in the source and docs only
to identify what the mod patches.
