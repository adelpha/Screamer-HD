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
| `--frames N` | Run exactly N simulation frames (70 Hz), then exit. Headless runs step once per loop, so output depends only on N, never on timing. |
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
* **Debugger**: `-Debugger` uses `dosbox_with_debugger.exe` (Alt+Pause breaks in).
* `-Cycles 'fixed 60000'` gives more repeatable timing than `max`. Audio is off unless `-Sound`.

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
