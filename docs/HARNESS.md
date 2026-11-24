# Test harness

Everything here runs unattended from a plain PowerShell on Windows. No IDE,
no manual steps.

## Prerequisites (already on the dev machine)

| Tool | Where | Notes |
|---|---|---|
| MSVC x64, CMake, Ninja | Visual Studio 2022 Build Tools | `tools/env.ps1` finds them via vswhere |
| SDL3 | fetched by CMake (`cmake/Dependencies.cmake`) | pinned to `release-3.4.20` |
| DOSBox Staging 0.83.1 | `tools/bin/dosbox-staging/` (git-ignored) | only for reference runs of the original |
| Original game | `SCREAMER/` (git-ignored) or `$env:SCREAMER_DATA` | never committed |
| Ghidra 12.1.4 | `ghidra_12.1.4_PUBLIC/` (git-ignored) | run via `tools/ghidra-headless.ps1`, which selects Temurin JDK 21 (default JAVA_HOME is Java 8) |
| Python 3.14 | `py -3.14` (not on PATH as `python`) | for data-parsing scripts |

## Build, run, test

```powershell
.\tools\build.ps1                    # debug build -> build\debug\bin
.\tools\build.ps1 -Preset release
.\tools\test.ps1                     # build + all tests
.\tools\test.ps1 -Label render       # labels: unit, render, smoke, gamedata
.\tools\run.ps1                      # windowed
.\tools\run.ps1 --headless --frames 140 --screenshot artifacts\shot.png
```

`screamer.exe` options useful for automation:

| Option | Effect |
|---|---|
| `--headless` | SDL `offscreen` video driver, software renderer, dummy audio. No window appears. |
| `--frames N` | Run exactly N simulation frames, then exit. A frame is 2 ticks of the original's 100 Hz clock (`src/core/sim_clock.h`); headless runs advance exactly 2 ticks per loop, so output depends only on N, never on timing. |
| `--scene NAME` | `title` (the title screen from the game files; the default when they are found) `track` (a track drawn by the new 3D renderer) or `testpattern`. The tests use `testpattern` so they run without game data. |
| `--track NAME` | Track for `--scene track` (default `MAP2`, loaded from `GFX/<NAME>L.DAT`). |
| `--camera X,Y,Z,YAW,PITCH[,ROLL]` | Camera for `--scene track` in game units (angles 0..2047). Default: the first placement. `48459,196,7124,472,2005,0` is the intro race at the start gate. |
| `--render-size WxH` | Framebuffer size for the 3D scene (default 320x200), e.g. `1280x800`. |
| `--flat` | Draw faces with each material's average colour instead of textures. |
| `--scene drive` | Drive a car on the track with the ported physics and the chase camera: arrow keys (up accelerate, down brake, left/right steer) or a gamepad (triggers, left stick). `--car-index N` picks the car from `CARS_DATA`, `--start X,Z` the start position, `--hold accel,left` holds controls in every frame for headless runs; the last frame logs the car's state. With CPU cars (tracks with racing lines, e.g. `--track MAP1`) the race starts with the countdown and shows the HUD; F1-F4 switch the view (bumper, cockpit, chase, far chase; `--view N` starts in one), M cycles the radar size. |
| `--detail high\|low` | Track files: `GFX/MAPnH.DAT` (default; what the game loads) or `MAPnL.DAT`. |
| `--car NAME[,X,Y,Z,YAW[,ANIM]]` | Add a car object (`AUTO00`..`AUTO06`) with its shadow; repeatable. `ANIM` is the object's animation word in hex (`8000` = second paint scheme). Without a position the car stands in front of the camera. |
| `--dump-fb FILE` | After the last frame, save the native 320x200 framebuffer (palette applied). Directly comparable with DOSBox raw captures. |
| `--screenshot FILE` | After the last frame, save the presented output (scaled, letterboxed) at `--size`. |
| `--capture-every N` / `--capture-dir` | Dump the framebuffer every N frames. |
| `--size WxH`, `--aspect 4:3\|stretch\|square`, `--renderer NAME`, `--fullscreen`, `--data DIR` | |

## Test types

* **unit** (`tests/unit/`): one executable, self-registering cases (`tests/testing.h`). Add a
  `test_*.cpp` file and list it in `tests/CMakeLists.txt`. Use `SKIP("...")` for cases that
  need the original game files so CI still passes without them.
* **render** (`tests/CMakeLists.txt`, `add_render_test`): runs `screamer --headless`, then
  compares the image with `tests/golden/*.png` using `imgcmp`. On failure the actual image
  and a diff (`*.diff.png`, differing pixels in red) are in `build/<preset>/test-output/`.
  After an intentional rendering change run `.\tools\update-golden.ps1`, look at the new
  images, and commit them.
* **smoke**: the executable starts and exits cleanly.
* **gamedata**: checks the local original files against `reference/manifest/screamer_files.sha256`
  (skipped when the files are absent).

## imgcmp

```
imgcmp expected.png actual.png [--tolerance N] [--max-diff-fraction F] [--diff out.png]
       [--resize-actual WxH | --resize-to-expected] [--crop X,Y,W,H]
```
Prints `RESULT match|differ size=... differing=... max_delta=... mae=...`; exit 0 = match,
1 = differ, 2 = error. Reads PNG and BMP.

## Reference runs of the original game (DOSBox Staging)

`tools/dosbox/run-original.ps1` starts the original in DOSBox Staging with a generated
config (user's own DOSBox config is ignored), mounts `SCREAMER/` as C: and the CD image as D:.

* **Input**: `-Autotype '<args>'` uses DOSBox's built-in `AUTOTYPE` command, which types into
  the emulated keyboard, so no window focus is needed. Example: `-Autotype '-w 6 -p 1 enter down enter'`
  waits 6 s then presses Enter, Down, Enter one second apart. Run `autotype -list` inside DOSBox
  for key names.
* **Screenshots**: `-Shots 8,12.5` posts the screenshot hotkey (remapped to F11) to the DOSBox
  window at those times. Captures are `raw`, i.e. the exact 320x200 framebuffer, saved as
  `artifacts/dosbox/<Name>/shot_NN_<t>s.png`. The DOSBox window does appear on screen but
  focus is not stolen.
* **Memory/CPU**: `-Webserver` enables the DOSBox REST API on `127.0.0.1:8086`. While running,
  `tools/dosbox/read-memory.ps1` reads memory (e.g. `-Address 0xA0000 -Length 64000` for the
  mode 13h screen) or `-Cpu` for registers. Works with the DOS4GW protected-mode game.
* **Headless**: `-Headless` sets SDL's offscreen video driver, so no DOSBox window appears
  (cannot be combined with `-Shots`; read memory through `-Webserver` instead).
* **Screen capture without a window**: `tools/dosbox/capture-vga.ps1 -Name x -Delay 9 -Autotype '-w 4 esc'`
  runs DOSBox headless, reads VGA memory and the game's own `g_palette`, and writes
  `artifacts/dosbox/x/vga.bmp`. The palette is the full-brightness one, so capture screens
  that have finished fading in.
  `-Freeze` (races and the intro demo) patches `JMP $` in after `render_race_scene`, so the game
  stops with the finished 3D scene in its back buffer; that buffer is captured instead of VGA,
  together with the memory it was drawn from. `-KeepMemory` keeps the dump (`mem_00.bin` holds
  the first MB, where the 3D object instances live). With both, camera (`g_camera`), car
  instances (`g_objects[].instance`: x, y, z, angles, animation word) and screen are from the
  same frame, so `screamer.exe --scene track --camera ... --car ...` can be compared with
  `imgcmp` directly.
* **Debugger**: `-Debugger` uses `dosbox_with_debugger.exe` (Alt+Pause breaks in).
* `-Cycles 'fixed 60000'` gives more repeatable timing than `max`. Audio is off unless `-Sound`.

* **Recording the original's car physics**: `tools/dosbox/record-race.ps1 -Out artifacts/traces/x.trace
  -Frames 450 -Script '170:accel,60:accel+left,40:brake' [-Skip N]` starts a new game (MAP1, default
  name) headless, then patches a small hook over the game loop's `CALL race_frame`: every frame it
  writes the scripted controls over `g_input_*` and copies the frame time, damping-table row, race
  flags and `g_player_car` to a buffer at 15 MB, read back at the end (`step_race.py`). `-Skip N`
  drives N frames before recording. Stepping through the web API instead (JMP $ traps) hangs
  DOSBox after a frame or two. `tracecmp TRACE GFX/MAP1H.DAT MAP1` then starts every frame from
  the recorded state, runs our `Car::update` and lists the fields that differ from the next record.
  With `RECORD_OPPONENTS=1` in the environment the recording also holds `g_opponents` (about 65
  frames fit); `tracecmp ... --data DATA/MAP1.DAT` then steps the whole race (player and CPU cars).

Comparing our output with the original:

```powershell
.\tools\dosbox\run-original.ps1 -Name menu -Autotype '-w 4 esc' -Shots 9 -Duration 10
.\tools\run.ps1 --headless --frames 500 --dump-fb artifacts\ours_menu.png
.\build\debug\bin\imgcmp.exe artifacts\dosbox\menu\shot_00_9.0s.png artifacts\ours_menu.png --diff artifacts\menu_diff.png
```

Timing in DOSBox is wall-clock based, so animated scenes will not line up frame-for-frame;
compare truly static screens exactly, use `--crop` to isolate static regions, and use
`--tolerance` / `--max-diff-fraction` for the rest. For exact state comparisons prefer reading
game memory through the REST API (below) over screenshots.

## Known observations

* The original (STARTL.EXE) boots to an intro (Virgin "PRESENTS", Graffiti logo) over a
  moving road background, in VGA mode 13h 320x200x256, within ~2 s of launch.
* `-Autotype '-w 4 esc'` skips the intro to the main menu (NORMAL GAME, CHAMPIONSHIP,
  SPECIAL MODES, NETWORK GAME, OPTIONS), reached by ~6 s. The menu animates (the
  background and title shimmer), so two captures of it are not pixel-identical.
* Several Escs in a row quit the game back to DOS.

## Reverse engineering

Ghidra import, the symbol map and the DOSBox memory probe are described in
[REVERSING.md](REVERSING.md).
