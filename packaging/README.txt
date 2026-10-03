Road Trip Overhaul v{VERSION}
for American Truck Simulator - Road Trip car mode ({GAME_VERSION})
https://github.com/PicSoul/road-trip-overhaul
====================================================================

Makes the cars of ATS's Road Trip mode drive more like real cars:

- Automatic shifting by throttle. Gentle driving shifts early, as the game
  does; more throttle holds each gear longer; full throttle holds each gear
  until the next gear pulls harder (worked out from the engine's torque
  curve). One gear at a time on the throttle, like a real automatic, and no
  hunting between gears when coasting.
- Real 2H / 4H on 4x4 cars (Bronco, F-150). The diff lock key (V) switches
  between rear-wheel drive (2H) and four-wheel drive (4H). The F-150's "4H"
  light and the Bronco's 4x4 lever follow it.
- Tyre grip depends on the surface: grass is slippery, dirt and gravel a bit
  less grippy than pavement.
- Keeps the game's own "adaptive automatic transmission" mode working in cars
  (upshifts when you lift off), and gives your own setting back in trucks.

Trucks are not affected unless you ask for it (vehicles=all).

Requires the Road Trip DLC for car mode. Nothing in the game's files or your
save is changed; remove the plugin and everything is back to stock.

OPTIONAL: Road Trip Overhaul - Vehicle Tuning
  A separate data mod that calibrates weight, drag, engine output and tyre
  grip of the Road Trip cars to their real-world acceleration and top speed.
  The plugin and the mod work on their own or together.
  https://github.com/PicSoul/road-trip-overhaul-vehicles


INSTALL
-------
1. Copy road_trip_overhaul.dll into the game's plugins folder:

     ...\steamapps\common\American Truck Simulator\bin\win_x64\plugins\

   (Create the "plugins" folder if it does not exist. In Steam: right-click
   the game > Manage > Browse local files > bin > win_x64.)

2. Start the game. It asks whether to allow "SDK plugins" - accept.

3. Press ~ to open the console. You should see:
     [Road Trip Overhaul] v{VERSION} active

Tip: turn off the game's "automatic differential lock" option, or it may
switch to 4H by itself.


IN GAME
-------
  Scroll Lock   switches the plugin on / off while you drive (a beep tells
                you which). Handy to compare with the game's own behaviour.
  V             (the diff lock key) 2H / 4H on 4x4 cars.


SETTINGS
--------
The plugin creates road_trip_overhaul.ini next to itself on first start.
Settings are read when the game starts. After an update, new settings are
added to your existing file automatically; your values are kept. Every
setting is explained in the file itself. The main ones:

  enabled=1            1 = on, 0 = off
  vehicles=car         car = only when driving a car, all = trucks too
  toggle_key=scrolllock
                       key to switch the plugin on / off, e.g. pause,
                       ctrl+shift+g, alt+f9; none = no key
  four_wd=1            1 = V switches real 2H / 4H on 4x4 cars
  lock_diffs=1         1 = differentials always locked (2H and 4H),
                       0 = locked only in 4H
  grip_grass=0.7       tyre grip on grass, dirt and pavement, as a
  grip_dirt=0.9        multiple of the game's own grip (1 = unchanged);
  grip_road=1.0        cars only
  four_wd_include=     vehicle ids that always get 2H / 4H (mod cars)
  four_wd_exclude=     vehicle ids that never do
  game_adaptive_mode=10
                       the game's adaptive automatic mode in cars:
                       10 = Eco, 3 = Normal, 1.67 = Power, 0 = leave it
  log=0                0 = no log file, 1 = log shifts and 0-60 times
                       (only needed for troubleshooting)

Shift tuning (light_throttle, full_throttle, full_upshift, release_time,
min_gear_time, max_upshift_power, max_upshift_light) is described in the ini.


MOD VEHICLES
------------
Shifting and surface grip work on any car, including mod cars. 2H / 4H works
on cars whose data drives both axles. Built in: the LORD G350 pickup mod
(vehicle.ford.350c), whose data is rear-wheel drive. Other mod cars can be
added with four_wd_include (the vehicle id is in the log with log=1).


AFTER A GAME UPDATE
-------------------
If the console says "[Road Trip Overhaul] ... INACTIVE", the update moved
code the plugin relies on. The plugin then does nothing at all (your game is
not affected). Check the GitHub page above for an updated version.


UNINSTALL
---------
Delete road_trip_overhaul.dll (and road_trip_overhaul.ini / .log) from the
game's bin\win_x64\plugins folder.


MULTIPLAYER
-----------
SCS Convoy: only affects your own vehicle.
TruckersMP has its own rules about client modifications and may treat
memory-modifying plugins as a violation. Use it there at your own risk.


License: MIT - see LICENSE.txt. Includes MinHook and SCS SDK headers - see
THIRD_PARTY_NOTICES.txt.
