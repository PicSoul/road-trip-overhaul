# Road Trip Overhaul

A telemetry SDK plugin for **American Truck Simulator** that makes the cars of the Road Trip car mode drive more like real cars.

- **Throttle-based automatic shifting.** Gentle driving shifts early, as the game does; more throttle holds each gear longer; full throttle holds each gear until the next gear pulls harder (worked out from the engine's torque curve). One gear at a time on the throttle, like a real automatic, and no hunting between gears when coasting.
- **Real 2H / 4H on 4x4 cars** (Bronco, F-150). The diff lock key (V) switches between rear-wheel drive (2H) and four-wheel drive (4H). The F-150's "4H" light and the Bronco's 4x4 lever follow it.
- **Surface-dependent tyre grip.** Grass is slippery, dirt and gravel a bit less grippy than pavement.
- **A parking brake that holds.** The game's car parking brake is so weak that you can drive off with it on. It now holds the rear wheels properly (also handy as a drift aid), or all four wheels like a truck's if you prefer.
- **The game's adaptive automatic mode** (upshift when you lift off) is used in cars; your own setting is given back in trucks.

Trucks are not affected unless you set `vehicles=all`. Nothing in the game's files or your save is changed; remove the plugin and everything is back to stock. Requires the Road Trip DLC for car mode.

## Optional: Road Trip Overhaul - Vehicle Tuning

A separate data mod that calibrates weight, drag, engine output and tyre grip of the Road Trip cars to their real-world acceleration and top speed. The plugin and the mod each work on their own, or together.

- **[Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3812313188)** (subscribe, then enable it in the in-game Mod Manager)
- [GitHub](https://github.com/PicSoul/road-trip-overhaul-vehicles) (source and manual download)

## Install

1. Download the zip from [Releases](https://github.com/PicSoul/road-trip-overhaul/releases).
2. Copy `road_trip_overhaul.dll` into `...\steamapps\common\American Truck Simulator\bin\win_x64\plugins\` (create `plugins` if it does not exist).
3. Start the game and accept the "SDK plugins" prompt. The console (`~`) shows `[Road Trip Overhaul] v1.0.0 active`.

Tip: turn off the game's *automatic differential lock* option, or it may switch to 4H by itself.

## In game

| Key | Action |
|---|---|
| Insert | Plugin on / off while you drive (a beep tells you which). Configurable with `toggle_key`. |
| V (diff lock) | 2H / 4H on 4x4 cars |

## Settings

`road_trip_overhaul.ini` is created next to the DLL on first start and explains every setting. New settings are added automatically after an update; your values are kept.

| Setting | Default | |
|---|---|---|
| `enabled` | `1` | 1 = on, 0 = off |
| `vehicles` | `car` | `car` = only when driving a car, `all` = trucks too |
| `toggle_key` | `insert` | key to switch on / off, e.g. `scrolllock`, `pause`, `ctrl+shift+g`, `alt+f9`; `none` = no key |
| `four_wd` | `1` | V switches real 2H / 4H on 4x4 cars |
| `lock_diffs` | `1` | differentials always locked (1) or only in 4H (0) |
| `grip_grass` / `grip_dirt` / `grip_road` | `0.7` / `0.9` / `1.0` | tyre grip per surface as a multiple of the game's grip (cars only) |
| `parking_brake` | `3` | parking brake strength in cars, as a multiple of the game's (1 = unchanged) |
| `parking_brake_all_wheels` | `0` | 0 = rear wheels, like a real handbrake; 1 = all four wheels, like a truck's (cars cannot be driven off with it on, even in 4H) |
| `four_wd_include` / `four_wd_exclude` | | vehicle ids that always / never get 2H / 4H |
| `game_adaptive_mode` | `10` | the game's adaptive automatic mode in cars: 10 Eco, 3 Normal, 1.67 Power, 0 = leave it alone |
| `log` | `0` | 1 = log shifts and 0-60 times (troubleshooting) |

Shift tuning (`light_throttle`, `full_throttle`, `full_upshift`, `release_time`, `min_gear_time`, `max_upshift_power`, `max_upshift_light`) is described in the ini.

## Mod vehicles

Shifting and surface grip work on any car, including mod cars. 2H / 4H works on cars whose data drives both axles. Built in: the LORD G350 (`vehicle.ford.350c`) and RVM (`vehicle.ram.3500c`) pickup mods by Jon Ruda, whose data is rear-wheel drive. Other mod cars can be added with `four_wd_include` (with `log=1` the vehicle id is in the log).

## How it works

The plugin hooks the game's per-gear shift range calculation and raises only the upshift point according to the throttle; the downshift point stays the game's own. 2H / 4H switches the chassis' powered axles and rebuilds the drivetrain when the diff lock key is used. Surface grip scales the friction of the game's grass, dirt and road surfaces while a car is driven and restores them otherwise. The parking brake hooks the game's per-wheel brake calculation and raises the parking brake torque of your car after the game's own result. Game code is found by signature scanning; if a game update moves it, the plugin stays inactive and the game runs stock.

## Building

Visual Studio 2022 (x64):

```
git clone --recursive https://github.com/PicSoul/road-trip-overhaul
build.bat      -> bin\road_trip_overhaul.dll
package.bat    -> dist\Road-Trip-Overhaul-v<version>.zip
```

## Multiplayer

SCS Convoy: only affects your own vehicle. TruckersMP has its own rules about client modifications and may treat memory-modifying plugins as a violation; use it there at your own risk.

## License

MIT - see [LICENSE](LICENSE). Includes [MinHook](https://github.com/TsudaKageyu/minhook) and SCS SDK headers - see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
