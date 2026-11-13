# Noiseflip

Reads the skater's **physics state**, the **board's motion** and your **gamepad** from skate. (run through the original, unmodified ReSkate) and turns 13 things into 13 MIDI notes plus 6 MIDI controllers. Nothing is injected and nothing is written to the game: memory is only read from outside.

Tested in the real game (Steam build 25414733) and by ~1.4 million automated checks (`tests.cpp`, mostly random-session fuzzing). `fake_skate.exe` plays a scripted session for testing without the game.

## Install

1. Install [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html) and create a port.
2. Unzip the release anywhere. Keep the `ui` folder next to `noiseflip.exe`.
3. Start skate. through ReSkate, then start `noiseflip.exe`. An app window opens (Edge or Chrome in app mode); closing it leaves Noiseflip running, start the exe again to reopen it. The window remembers its size and position (`noiseflip.window`).
4. In Ableton: Settings > Link, Tempo & MIDI > the loopMIDI input: **Track = On**, **Remote = On**.

First run creates `noiseflip.ini`; every change in the UI is saved to it at once. Editing it by hand while running also works (reloads within a second).

## The UI

| Tab | Contents |
|---|---|
| 01 GLOBAL | MIDI channel, velocity, speed bonus, gamepad triggers, manual stick range, landing velocity, shared rotation settings |
| 02 TRIGGERS | one fold per trigger: enabled, pulse/gate, velocity, delay, lengths, rotation settings |
| 03 CC | one fold per controller with a live value bar |
| 04 MAPPING | note of every trigger, number of every controller, channels, and a **TEST** button that sends that signal on repeat so Ableton can learn it (Ctrl+M, click target, press TEST) |
| 05 LIVE | physics state, speed, height, board spin, gamepad, triggers, controllers, console log |
| 06 PRESETS | save/load the whole setup under a name (`presets/<name>.ini`), undo last load |

Settings that never need changing (physics state lists, game process, gamepad slot, push/footplant/no-foot-grab buttons, manual stick axis, ignore/extra states) are hidden from the UI. They keep their values in `noiseflip.ini`.

The UI is served by the program on `127.0.0.1:8431` (loopback only, Host header checked, every change needs a header other web pages cannot send). Options: `--no-ui`, `--no-window`, `--ui-port N`.

## Triggers (defaults)

| Trigger | Kind | Condition | Note |
|---|---|---|---|
| jump | pulse | ground to air | 36 |
| land | pulse | air to ground; velocity follows the drop | 37 |
| push | pulse | A or X on the ground, not with a grab; fires 500 ms later | 38 |
| bail | pulse | into state 504, then disarmed until ridden 500 ms | 39 |
| grind | gate | state 400-499 | 40 |
| manual | gate | ground, right stick 13000-30000 for 200 ms | 41 |
| grab | gate | LT or RT, not in a manual/grind, after 200 ms | 44 |
| footplant | pulse | grab + A or X | 45 |
| nofootgrab | pulse | grab + B | 46 |
| rot_x / rot_y / rot_z | pulse | board rotation (angle mode: a note per 180 degrees) | 48 / 49 / 50 |
| fingerflip | pulse | grab held + board turning in the air | 51 |

Every trigger can be switched between **pulse** (fixed-length note) and **gate** (held while the condition lasts) with `mode`.

## MIDI controllers (CC 20-25)

intensity (energy meter), rot_x / rot_y / rot_z (net spin as faders that stay after landing), manual (stick position), speed (board speed). All tunable in the `[cc_*]` sections. `--no-cc` turns them off.

## Rotation

Rates are the board root body's angular velocity in world axes, in degrees per second (tricks are roughly 900-1200, ollies under 100). Two modes, switched by `[rotation] detect` or by clicking the left stick while playing:

* **angle** (default): a note for every `step_deg` (180) degrees, per axis. Kickflip = 2 notes. Ground turns count only above `ground_min_rate`, so carving is silent but a **revert** plays the shuvit note (rot_y).
* **rate**: one note when the spin rate passes `threshold`.

`noiseflip.exe --watch` prints live speed, height and spin to tune thresholds.

## Command line

```
--config FILE  --write-config [--force]  --list  --midi NAME  --midi-index N
--channel N  --velocity V  --hold MS  --poll-ms N  --pad N  --process NAME  --pid N
--pad-test  --watch  --detect rate|angle  --test-midi  --dry-run  --log FILE  -v
--midi-trace  --note-off noteoff|velocity0  --no-midi-check  --allow-multiple
--test-cc  --test-cc20  --test-note37  --map  --no-cc  --no-ui  --no-window  --ui-port N
```

## Unexpected notes

Run `noiseflip.exe -v --midi-trace`. If the trace shows only your note but Ableton shows more, the extra notes come from a MIDI effect inside Ableton. A `FOREIGN MIDI` line means another program sends to the same port. If the trace itself shows the extra note, the game's physics state flickered (the `-v` lines show the numbers).

## Build

`./build_mingw.sh` (MinGW-w64) or `build_msvc.bat` (x64 Native Tools prompt). C++17. Both compile `noiseflip.rc` (icon + version info) and run the tests. `make_release.sh` / `make_release.bat` build and zip a release.

Release contents:

```
noiseflip.exe
ui\   index.html, app.js, style.css, icon.png, favicon.ico, logo.png, fonts\Archivo.ttf, fonts\JetBrainsMono.ttf
```

## Limits

* One game build only (Steam **25414733**). After an update the RVAs in `skate_chain.h` must be replaced.
* It polls about every 1 ms; a state shorter than a poll can be missed.
* "Manual" means ground + stick in range + delay; the game's own manual state is not used.
* Stance and trick names are not read.
* Not affiliated with EA, Full Circle or ReSkate. The offsets come from ReSkate's source (GPL-3.0): choose a licence deliberately before publishing.
