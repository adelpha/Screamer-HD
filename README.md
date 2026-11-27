# Screamer recompilation

A reconstruction of the 1995 DOS racing game *Screamer* (Graffiti / Virgin Interactive)
as a native Windows program built on SDL3, with the goals of high resolutions, widescreen,
3D acceleration, better audio options and controller support.

**No original game files are included.** You need your own copy of Screamer. Point the
build at it by placing it in `SCREAMER/` next to this file (git-ignored) or by setting
`SCREAMER_DATA` to its folder.

## Status

Early scaffolding: the SDL3 frontend, 8-bit framebuffer/palette pipeline and the test
harness exist; the game itself is not reconstructed yet (a test pattern is drawn instead).

## Building (Windows)

Requires Visual Studio 2022 (or its Build Tools) with the C++ workload. The dependencies are
fetched automatically at pinned versions:

- [SDL3](https://github.com/libsdl-org/SDL) (zlib license): window, graphics, audio, input.
- [Nuked-OPL3-fast](https://github.com/tgies/Nuked-OPL3-fast) (LGPL-2.1): the OPL3 FM chip
  that plays the MIDI music. It is built as its own `opl3.dll`, so it can be swapped for any
  build of the same source.

```powershell
.\tools\build.ps1            # debug
.\tools\test.ps1             # build + run tests
.\tools\run.ps1              # play
```

See [docs/HARNESS.md](docs/HARNESS.md) for the test harness, headless runs, golden-image
tests and running the original game in DOSBox Staging for reference comparisons.

## Controls

Driving (defaults; OPTIONS > CONTROLS rebinds them, saved in `CONTROLS.INI` next to
`CHOICE.DAT`):

| Action | Keyboard | Gamepad |
|---|---|---|
| Accelerate | Up | right trigger |
| Brake | Space, Down | left trigger |
| Steer | Left / Right | left stick, D-pad |
| Gear up / down (manual gearbox) | Page Up / Page Down | Y / X |
| Look back | B | right shoulder |
| Car stereo (pause) | P | Start |
| End the race | Esc | Back |

The first time, the keys from the game's own `CONFIG\CONTROLS.CFG` (SETUP.EXE) come over as
each action's first key (e.g. Alt / Ctrl for the gears), with ours as the second.
Gamepads and other joysticks (wheels, pedals) can be plugged in and out at any time; any of
them drives, or OPTIONS > CONTROLS > PAD picks one. Every action takes two keys and two
controller inputs: a button, a hat direction or an axis direction (pedals resting at one end
use their whole travel); on the CONTROLLER page left / right switches an action's axes
between analog and digital (read as a key, like the original's "joystick as keys"). RUMBLE
ON/OFF (applies at once). DEFAULTS puts the bindings back. On the KEYBOARD and CONTROLLER
pages Enter on an action waits for its new key or input (Esc, or the pad's Back, cancels;
the race's own keys P, M and F1-F12 are not taken); the new one comes first and the old first
second, and catching the one already first again leaves it alone.

The menus, the car stereo and PLAY AGAIN? always answer to the arrows, Enter and Esc, and to
the D-pad / left stick, A (confirm), B (back) and Start, whatever the bindings. On the main
page the pad's Back quits the game (as Esc does); a pad without Back quits when B is held for
2 seconds (a ring fills in the bottom right corner, shown once B has been held 0.2 s, so a
tap shows nothing). Tapping B there does nothing.

Whatever is bound, the game gets its inputs as the original's controls gave them (keys 1023;
axes through its clamp and dead zone; the brake set straight from a pedal, ramped from a key;
the stick as the steering angle, keys ramped), so the driving and replays stay the
original's.

## Layout

```
src/core/        engine code independent of the frontend (framebuffer, data files, images)
src/main.cpp     SDL3 frontend
src/tools/       imgcmp image comparison tool
tests/           unit tests, render tests, golden images
tools/           build/test/run scripts, DOSBox reference harness
reference/       hashes and notes about the original (never the original files)
docs/            documentation
```
