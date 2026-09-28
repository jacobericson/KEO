# KEO: Kenshi Engine Optimizations

## THIS MOD IS STILL IN BETA

> **Beta.** This mod is still in testing. Stability is not guaranteed. The mod's files still use its earlier name, `KenshiZoneOpt` (`KenshiZoneOpt.dll`, `KenshiZoneOpt.ini`, `KenshiZoneOpt.log`).

A performance and stability mod for Kenshi, built as an RE_Kenshi plugin.

KEO makes moving between areas of the map smoother. It loads areas before your squads reach them, gets pathing for new ground ready sooner, and adds frame-rate optimizations that you can switch on and off individually.

- **Area transitions:** about 94 % faster on a first visit (2.5 s → 0.15 s on average), with the world no longer freezing while you travel (58 s of freeze over a test walk → none).
- **Frame rate:** 13 to 19 fps higher in a swamp, 4 to 7 fps in a town, and 1.5 to 10 fps in a crowded desert town, with every render optimization on against all of them off.

## Features

- **Faster area loading.** Areas around your camera and your squads are loaded ahead of time, so crossing into a new one is much quicker.
- **Pathing ready sooner.** Walkable ground for a new area is prepared faster and saved to disk, so an area you have visited before is ready right away.
- **Better squad movement.** Long move orders the game drops partway are re-issued, and squads keep together while travelling.
- **Higher frame rate.** Render and particle optimizations. Any change that causes a noticeable drop in quality is off by default.
- **Stability fixes** for several of the vanilla bugs and crashes.
- **ZoneOpt tab** in the game's Options window, with an **in-game benchmark** that measures the frame-rate optimizations on your own machine.

## Requirements

- Kenshi 1.0.65 (Steam).
- [RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) ([Nexus](https://www.nexusmods.com/kenshi/mods/847)) 0.3.5 or later.

## Installation

- **[Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3802456502):** subscribe, and enable the mod in the launcher.
- **Manual:** download the latest release, copy the `KEO` folder into Kenshi's `mods\` directory, and launch Kenshi through the RE_Kenshi launcher.

## Configuration

Every setting is optional and has a default. There are two ways to change one:

- the **Options → ZoneOpt** tab in game;
- `KenshiZoneOpt.ini` in the mod folder, where each setting is explained above its line. Some settings take effect only after a restart; the file says which.

## Known defects

- **GOG version not supported.** On the GOG build of Kenshi, KEO stays inactive: it detects the version at startup and changes nothing.

## Bug reports

If you run into a problem, [open an issue](../../issues/new) and include:

- `KenshiZoneOpt.log` and, if present, `crash_dump.txt`, both from the mod's folder;
- `RE_Kenshi_log.txt`, from the Kenshi game directory;
- the list of other mods you have enabled;
- what you were doing when it happened (crossing into a new area, a long move order, loading a save, …).

## Building from source

Requires Visual Studio 2010 (v100 x64 toolset, default install path), Python 3.7 or later, the [KenshiLib v0.5.1](https://github.com/BFrizzleFoShizzle/KenshiLib/releases/tag/v0.5.1) release archive and the Boost 1.60.0 headers.

```bat
set KENSHILIB=C:\path\to\KenshiLib
set BOOST_ROOT=C:\path\to\boost_1_60_0
build_opt_step4.bat
```

The mod is built into `build\KenshiZoneOpt_step4_prod\`. To install it, copy that folder into Kenshi's `mods\` directory. `build.bat` builds the optional `KenshiZoneProfiler` diagnostics plugin, and `tools\tests\build_tests.bat` runs the unit tests.

## License

GPL-3.0; see [LICENSE](LICENSE).
