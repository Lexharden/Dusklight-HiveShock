# Changelog

Versioning: [SemVer](https://semver.org). The version lives in `mod.json`; tag releases as `vX.Y.Z`.

## Unreleased

### Added
- Linux (x64, arm64) and macOS (Apple Silicon, Intel) builds.
- Forensic log and crash report on Linux/macOS: written to the mod's data folder (`<Dusklight config>/mod_data/com.hiveshock.interactive/`). Its exact path is printed in the Dusklight log at startup.

### Fixed
- The crash handler is removed on shutdown (and when initialisation fails), so a later crash after disabling the mod no longer jumps into an unloaded library.
- Windows: after writing `hiveshock_crash.log`, the crash is passed on to Dusklight's own crash reporter instead of replacing it.

## 1.2.0

### Added
- Spawn diagnostics: the log now says whether a spawn "NEVER EXISTED" (its `create()` failed) or died/despawned normally, plus stage/room/position/params and game/zelda heap free sizes.
- `poe` now spawns E_HP, the small lantern ghost (alias `espectro`), instead of E_PO, the large humanoid Poe. The command name is unchanged so existing profiles keep working.
- `hs_status` command: logs stage, room, tracked/pending spawns and heap state.

### Changed
- Gibdo no longer needs an engine change: before the mod deletes a Gibdo mid-scream it lets it finish the scream, so the engine's static `m_cry_gi` is never left dangling. Requires no modification of Dusklight.
- JSON commands are parsed with a real (flat) JSON reader: escapes are decoded, keys inside other values are no longer matched, numbers may be sent as strings.
- Pending army/swarm spawns are dropped when Link changes stage/room.

### Removed
- The large humanoid Poe (E_PO) is no longer spawnable.
- `invert`, `freeze_time`, `resume_time`: they only logged and reported success without doing anything.

### Fixed
- The version printed in the log now matches `mod.json`.
- Removed the engine-side Gibdo fix (`d_a_e_gi.cpp`) mentioned in 1.1.0; it is not part of released Dusklight.

## 1.1.0

### Added
- Mod icon (`res/icon.png`) shown in the in-game mod manager.

### Fixed
- Poe: spawned with `mArg0 = 0` (scripted Hyrule Castle Poe), whose create() aborts without zone switch 0x43. Now uses `mArg0 = 1` (field Poe).
- Poe: aggro was written through raw GameCube offsets, corrupting memory on x64 and crashing a few frames after spawn. Moved to `po_aggro.cpp`, which uses the real `e_po_class`.
- Gibdo: stale static `m_cry_gi` after a Gibdo was deleted left every later Gibdo unable to attack (engine fix in `d_a_e_gi.cpp`).
- Tracked spawns are now cleared on stage/room change, so allies no longer follow Link across places.
- Spawn cap no longer blocks new spawns: dead entries are pruned, stuck creates are dropped after ~10 s, and the oldest spawn is recycled when full.

## 1.0.0
- Initial release.
