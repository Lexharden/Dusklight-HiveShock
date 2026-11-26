# HiveShock

Interactive stream mod for Dusklight. Receives commands over TCP (port 43000) and turns them into in-game effects: enemy spawns, player stat changes, transformations and more.

## Protocol
One command per TCP connection, newline-terminated. Either JSON:

```json
{"action":"bokoblin","user":"Viewer","amount":5,"damage":8}
```

(`action`/`command`/`code`, `user`/`sender`, `value`/`amount`/`duration`/`strength`, `damage`), or `COMMAND_ID|SENDER|AMOUNT|PAYLOAD`.

## Commands
- Spawns: `bokoblin`, `keese`, `fire_keese`, `ice_keese`, `guay`, `bulblin`, `lizalfos`, `chuchu`, `tektite`, `skulltula`, `helmasaur`, `helmsaurus`, `wolfos`, `poe` (alias `espectro`), `gibdo`, `keese_twilight`, `shadow_beast`, `twilight_insect`, `twilight_vermin`, `poison_mite`, `bee`, `darknut`, `puppet_zelda`, `freezard`, `leever`, `cucco`, `goat`, `random_enemy`
- Groups: `bokoblin_army`, `cucco_army`, `goat_army`, `bee_swarm`, `clear_enemies`
- Items: `spawn_rupee`, `spawn_heart`, `spawn_fairy`, `recovery_fairy`, `great_fairy`, `spawn_pot`, `spawn_bomb`
- Player: `heal`, `damage`, `kill`, `rupees`, `steal_rupees`, `fill_rupees`, `fill_oil`, `empty_oil`, `knockback`, `explode`
- Transformation/equipment: `wolf_toggle`, `force_wolf`, `force_human`, `iron_boots_toggle|on|off`, `summon_epona`, `dismount_horse`
- Game state: `reset`, `delete_save` (`wipe_save`, `nuke_save`)
- Diagnostics: `hs_status`

`damage` never kills (leaves at least 1 heart-quarter); use `kill` for that.

## Security
The server currently listens on all interfaces without authentication, and `delete_save`/`reset`/`kill` are available to anyone who can reach the port. Only run it on a trusted network or behind a firewall until this is tightened.

## Compatibility
Built against the public Dusklight SDK and engine headers; it does **not** modify the engine. Build it against the same Dusklight version you run it on, since the mod reads actor structs directly.

## Install
Download the `.dusk` for your platform from [Releases](../../releases) and copy it to the Dusklight mods folder:

- Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
- Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
- macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`

A native `.dusk` carries the library of the platform it was built on, so each release has one file per platform
(`HiveShock-Dusklight-<version>-<platform>.dusk`): `win-x64`, `linux-x64`, `linux-arm64`, `macos-arm64` (Apple
Silicon) and `macos-x64` (Intel).

On macOS the first start may ask whether Dusklight can accept incoming network connections; allow it, or the
TCP port won't be reachable.

## Logs
Besides the Dusklight log, HiveShock writes `hiveshock_forensic.log` (a trace of every command) and, if the game
crashes, `hiveshock_crash.log` (last 16 commands, registers, fault address):

- Windows: next to `dusklight.exe`.
- Linux/macOS: the mod's data folder, `<Dusklight config>/mod_data/com.hiveshock.interactive/`. The exact paths are
  printed in the Dusklight log at startup.

On Windows a crash inside an effect is caught and the game keeps running. Linux and macOS cannot recover from a
fault the same way: the crash is logged and the game closes.

## Build
```
cmake -B build -S .
cmake --build build --config Release --target hiveshock_package
```
Output: `build/mods/hiveshock.dusk`. The Dusklight SDK is downloaded automatically at the commit pinned in
`CMakeLists.txt` (`DUSKLIGHT_VERSION`). To use a local checkout: `-DDUSKLIGHT_DIR=<path>`.

## Releasing
1. Bump `version` in `mod.json` and add a `## X.Y.Z` section to `CHANGELOG.md`.
2. Commit, then `git tag vX.Y.Z && git push --tags`.
3. The `Release` workflow checks the tag against `mod.json`, builds, and publishes the Release. The Hive website
   picks it up automatically through `notify-web.yml`.

To add a platform, uncomment its row in the matrix of `.github/workflows/release.yml` and `build.yml`.
