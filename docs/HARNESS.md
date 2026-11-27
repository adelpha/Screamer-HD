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
| `--ui low\|high\|auto` | HUD and menu graphics: the original's 320x200 set, STARTH.EXE's 640x400 set (`GAMEH.*`, `*_H.PKC`, `COCKnn?H`, `MENUH.*`, `*H.PCX`), or auto (default): 640x400 when the render size has 400 lines or more. The 640x400 set is drawn the way STARTH.EXE does it: every position doubled. |
| `--threads N` | Threads that draw the 3D view (default: one per CPU thread). The picture is the same for any number. |
| `--fill sse2\|avx2` | Which build of the pixel filler runs (`src/core/render3d_fill.inl`, built for both; default: AVX2 when the CPU has it). Both give the same picture. |
| `--mipmaps on\|off` | Draw squeezed (distant) textures from smaller versions averaged from them (`src/core/mipmaps.h`; halved along u and v separately, matched to the palette). Steadier and softer in the distance. On by default; F10 switches in a race. `--mip-sharpness X`: a level is used once a pixel covers 1/X of its texels (default 1.4; 1 sharper, 2 softer). |
| `--seal-edges all` | Road pieces reach past every edge of every face, as they did before; now only past the edges a face shares with no other face of its part (the pieces' outlines), and the painted lines (long narrow faces) are kept about a pixel wide where they get thinner. With `all` the faces beside a line paint over it and it breaks into dashes and specks. |
| `--texture-edges wrap` | Texel coordinates wrap at the edges of every polygon, as before; now polygons whose corners all lie inside the texture stay on its edge texels. |
| `--track-edits off` | Leave out the corners moved by hand in photo mode's edit mode (Tab in F8; `core/track_edit.h`). They are kept in `TRACKS\<track><detail>.TXT` next to CHOICE.DAT (headless runs: none) and only move what is drawn, like the welding. `--track-edits-file F` uses (and saves to) file F instead, also in headless runs. |
| `--weld-joints off` | Road pieces where the track file puts them. The files place the road as pieces with whole-number positions and angles, and neighbouring pieces miss each other by some twenty units, so the painted lines step sideways at every joint; by default the corners of neighbouring pieces are drawn half way between (`weld_road_joints` in `core/track.h`; drawing only, the physics uses the file's vertices). `--seal-edges all --texture-edges wrap --weld-joints off --mipmaps off` is the picture of the renderer before these four (what `render-compare.ps1` baselines from before then need as `-ExtraArgs`). |
| `--smooth-turn off` | Camera, car and sky angles in whole 2048ths of a turn, as the original keeps them. By default they keep their fraction and the sky follows the yaw exactly: on a large picture a slow turn otherwise moves the scene nearly two pixels at a time and the sky a whole panorama column. Drawing only. |
| `--folded-faces off` | Faces that are not flat (a few cliff faces are folded along a diagonal) are tested for facing the eye as a whole by their one normal, as the original does; that leaves a hole where such a face is skipped although half of it faces the camera. By default each half is tested on its own. |
| `--face-lists off` | Draw every face that faces the eye instead of only those in the part's draw order for the viewing direction (no visible difference in the compare scenes). |
| `--ground-obstacles off` | Cones, barriers and signs where the original draws them: their models are centred on their middle and stand with it at the ground, so the base is below the road and seems to slide along it as the camera nears. By default they are drawn raised, base on the road (the picture only). |
| `--sky-tilt off` | The sky panorama stays level when the camera rolls (a banked car), as it was drawn before; by default it turns about the projection centre with the rest of the scene. |
| `--hidden` | Like `--headless` (fixed steps, no sound device), but with the real video driver and renderer (Direct3D on the graphics card) on a window that is never shown. For checking and timing what a real run does: `--screenshot` reads the picture back from the graphics card, `--perf` shows the real upload and present times. |
| `--video-api sdl\|opengl` | How the finished picture reaches the window (`src/video/presenter.h`): SDL's renderer (default) or our own OpenGL 3.3 core context, which looks the palette up in a shader and scales as SDL's path does (the same picture; the `gpu` tests compare them). OpenGL needs a real video driver: with `--headless` it falls back to SDL's renderer, so use `--hidden`. OPTIONS > VIDEO > ADVANCED > API switches live by making the window again. |
| `--upload index\|rgba` | How the picture gets to SDL: as an 8-bit texture with the palette (default; the graphics card looks the colours up) or converted to RGBA first (the old way; still used with SDL's software renderer, which scales RGBA faster). Same picture. |
| `--perf` | Profile the main loop (core/profiler.h): pictures and game steps per second, and the time per picture in each part (events, game step, blending, 3D geometry, 3D fill, HUD, texture upload, present), every 5 seconds and at exit. |
| `--view-distance X` | How far the 3D view reaches, in multiples of the original's far clip on HI detail (43,808 units); the last quarter fades out in a 4x4 ordered dither. Default 2; 0 draws everything. In a race F9 / Shift+F9 step through 1, 1.5, 2, 2.5, 3, 3.5, 4, 5, 6, 7, 8. |
| `--tween-test` | Headless: one tick per loop, so every other picture is drawn between game steps (checks the blending; with `--perf`, measures the cost per picture). `--capture-from N` starts `--capture-every` at frame N. |
| `--lod` | Draw distant objects in the original's simpler versions (from 4000/7000 units, cars from 12000/16000). Off by default: full detail everywhere. |
| `--reversed` | Drive the track the other way (the menu's tracks 7-12): racing lines and the rest from `DATA/MAPnI.DAT`. |
| `--race-options N` | The cheats' `g_race_options`: 1 / 2 the other obstacles (MONTY / JOINT), 4 the clock stops (CLOCK), 0x10 mirror world (MIRRO), 0x20 upside down (UPDOW). In the menus the cheats are typed as words. |
| `--replay FILE` | Play a `.RPL` replay back: the race runs again from the recorded frame times and inputs, seen from the trackside cameras of `MAPnRPL.SPL` (F1-F4 switch to the usual views). The file's header gives car, laps, level, mode and options; `--track` gives the map (`REPLAY<t><n>.RPL` is map t+1). |
| `--record FILE` | `--scene drive`: save the race as a `.RPL` when the run ends (playing it back with `--replay` must end at the same spot). |
| `--save-dir DIR` | Keep `CHOICE.DAT`, ghosts and `REPLAYS/` in DIR instead of the user's pref path (headless runs save nothing without it). |
| `--flat` | Draw faces with each material's average colour instead of textures. |
| `--scene drive` | Drive a car on the track with the ported physics and the chase camera, with the controls of OPTIONS > CONTROLS (`src/input.h`; defaults: Up accelerate, Space or Down brake, Left/Right steer, Page Up/Down gears; a gamepad's triggers, left stick or D-pad, Y/X gears; see the README). `--car-index N` picks the car from `CARS_DATA`, `--start X,Z` the start position, `--hold accel,left` holds controls in every frame for headless runs (`accel`, `brake`, `left`, `right`, `gearup`, `geardown`); headless runs use the default bindings and save none; the last frame logs the car's state. With CPU cars (tracks with racing lines, e.g. `--track MAP1`) the race starts with the countdown and shows the HUD; F1-F4 switch the view (bumper, cockpit, chase, far chase; `--view N` starts in one), M cycles the radar size. B (or the gamepad's right shoulder button) held looks back: the camera faces the other way, and in the chase views goes to the other side of the car (the picture only; `--hold rear` for headless runs). F8 (in a race) is photo mode: the race freezes and goes quiet, the HUD hides and a free camera flies about (WASD, Q/E or Space/C up/down, mouse or arrows to look, Shift faster, Ctrl slower; F8 or Esc to return; `--hold freecam` enters it at frame 150 flying forward). Tab in photo mode is the track edit mode (see the README); `--hold edit` enters it at frame 150 looking down at the widest open gap, `--hold editmeet` also makes that gap's corners meet (saved to `--track-edits-file`). F12 (in any scene) saves the frame as drawn, at the render size, to Pictures/Screamer/screamer_DATE_TIME.png. |
| `--scene menu` | The front end (the default when the game files are found): main menu, options, then NORMAL GAME goes name, track and car selection into a race and back. `--menu-keys 200:enter,300:down` presses keys at given frames for headless runs (`200-300:key` holds one over those frames; `padback` is a pad's Back, `padb` B on a pad without Back: held 2 s on the main page it quits, the ring in the corner showing from 0.2 s). In a race from the menu, Esc or the end of the race asks PLAY AGAIN?; P opens the car-stereo panel: previous/next switch the CD track or MIDI tune at once (MUSICn.HMP shows as track n+1), eject plays the track from the other source (CD <-> MIDI, saved as OPTIONS > SOUND's CD MUSIC / MIDI MUSIC), MODE picks one of OPTIONS > SOUND's four volumes (AN, EN, FX, MU) and VOL -/+ step it as that page does (saved). The music plays on while it is up. `--hold pause` opens it at frame 150 in headless runs; `--hold pausevol` also presses previous, VOL - on AN, MODE, VOL - on EN and eject (add `--record-audio` so there is music). |
| `--game-mode N` | `--scene drive` in a special mode: 4 time attack, 0x20 cone carnage, 0x40 slalom (alone, their obstacles). |
| `--detail high\|low` | Track files: `GFX/MAPnH.DAT` (default; what the game loads) or `MAPnL.DAT`. |
| `--car NAME[,X,Y,Z,YAW[,ANIM]]` | Add a car object (`AUTO00`..`AUTO06`) with its shadow; repeatable. `ANIM` is the object's animation word in hex (`8000` = second paint scheme). Without a position the car stands in front of the camera. |
| `--dump-fb FILE` | After the last frame, save the native 320x200 framebuffer (palette applied). Directly comparable with DOSBox raw captures. |
| `--screenshot FILE` | After the last frame, save the presented output (scaled, letterboxed) at `--size`. |
| `--capture-every N` / `--capture-dir` | Dump the framebuffer every N frames. |
| `--size WxH`, `--aspect 4:3\|stretch\|square\|wide`, `--renderer NAME`, `--fullscreen`, `--data DIR` | `wide`: widescreen. Square pixels on a picture of any shape (as large as the window unless `--render-size` says otherwise): races show more to the sides, the HUD keeps to the screen edges, menus stay 4:3 in the middle. Without it nothing changes. |
| `--windowed`, `--vsync on\|off`, `--frame-limit N` | OPTIONS > VIDEO (and its ADVANCED page) keeps fullscreen, widescreen, the render size (RES NATIVE: the window's pixels), view distance, detail, vsync, frame limit, far models (LOD) and mipmaps in `CHOICE.DAT` after the volumes. Interactive runs start with them; the command line's `--fullscreen`/`--windowed`, `--aspect`, `--render-size`, `--view-distance`, `--vsync`, `--frame-limit`, `--lod`, `--mipmaps` win for that run. Headless runs ignore the saved ones. Every change takes effect at once (the menu's picture is made again at the new size; a NATIVE menu also follows window resizes); a render size larger than the window is filtered down (supersampling). |
| `--record-audio FILE.wav` | Write the run's sound (effects, announcer and CD music) to a 44.1 kHz WAV, one 1/50 s block per frame, so headless runs give the same file every time. |
| `--music-source cd\|midi\|auto` | Music from the CD image, or the MIDI versions in DATA/MUSIC.DAT on the built-in FM synth (auto: CD when the image is found). `cd` and `midi` override the game's own setting, OPTIONS > SOUND > CD MUSIC / MIDI MUSIC (saved in `CHOICE.DAT` after the volumes; CD MUSIC behaves as `auto`). |
| `--audio-log` | Log every sample that starts, with its frame: `frame 300: sound 19 go`. |
| `--mute`, `--sfx-volume N`, `--music-volume N`, `--no-music`, `--no-speech`, `--audio-filter linear\|nearest` | Sound options (volumes 0..100: the car-stereo panel's FX and CD/MU levels). The music is the CD audio, read from the CD image in the game directory (`SCREAM.cue`/`.bin`, or `SCREAM.DAT`/`SCREAM.gog` as the Steam and GOG releases name them). |
| `--volumes A,E,F,M` | The OPTIONS > SOUND settings for this run, 0..10 each: announcer, the player's engine, all other effects (CPU engines, skids, crashes, scenery, menu clicks) and music. Without it they come from `CHOICE.DAT`, which keeps them after the original's 0x460 bytes (`SCRX`, a count, then the values); the original's own file gives 10 for all. |

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
* **gpu** (`add_api_test`, `tests/cmake/compare_apis.cmake`): runs `screamer --hidden` (the
  graphics card, a window that is never shown) once with `--video-api sdl` and once with
  `opengl` and compares the two screenshots; skipped where there is no OpenGL 3.3. Also the
  menu's live API switch.
* **gamedata**: checks the local original files against `reference/manifest/screamer_files.sha256`
  (skipped when the files are absent).

## Renderer changes: same picture, how fast

The 3D pictures come from the game's data, so there are no golden images of them in the
repository. A change that must not alter the picture is checked against the build before it:

```
.\tools\render-compare.ps1 -Exe <old screamer.exe> -Out artifacts\perf\base    # before (keep a copy of the old bin folder)
.\tools\render-compare.ps1 -Out artifacts\perf\new -Against artifacts\perf\base  # after: "all scenes identical"
```
It runs 23 headless scenes (a race on every track at 1600x1000, odd sizes, the original size,
the view-distance, detail, flat and cheat options, every view, pictures between steps, time
attack, cones, the intro demo, the menu, a still with cars, a whole replay), saves about 480
frames and the game's state at the end, and compares them file by file. `-ExtraArgs` adds
options to every run (`'--fill','sse2'`, `'--threads','1'`), `-Filter` picks scenes.
`tests/unit/test_render_fill.cpp` does the same in small without game data: a made-up scene
through every build of the filler, with the hashes of the pictures the renderer gave before
its fill loop was rewritten.

`.\tools\render-bench.ps1 [-Exe a.exe, b.exe] [-Runs 5] [-Size 1600x1000] [-ExtraArgs ...]` times a
headless race with `--perf` and prints the best and median per part of the loop; several
builds are run turn by turn. (Headless, "texture upload" and "present" are SDL's software
renderer; use `--hidden --perf` for the graphics card's.)

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

  .\tools\dosbox\record-race.ps1 -Out artifacts\traces\gears.trace
  -Frames 450 -Script '170:accel,60:accel+left,40:brake' [-Skip N]` starts a new game (MAP1, default
  name) headless, then patches a small hook over the game loop's `CALL race_frame`: every frame it
  writes the scripted controls over `g_input_*` and copies the frame time, damping-table row, race
  flags and `g_player_car` to a buffer at 15 MB, read back at the end (`step_race.py`). `-Skip N`
  drives N frames before recording. Stepping through the web API instead (JMP $ traps) hangs
  DOSBox after a frame or two. `tracecmp TRACE GFX/MAP1H.DAT MAP1` then starts every frame from
  the recorded state, runs our `Car::update` and lists the fields that differ from the next record.
  With `RECORD_OPPONENTS=1` in the environment the recording also holds `g_opponents` (about 65
  frames fit); `tracecmp ... --data DATA/MAP1.DAT` then steps the whole race (player and CPU cars).
  Player-only recordings (trace version 5) keep the control setup (`g_control_bindings`) and the
  gear inputs too, so the analog paths can be checked: `-Setup analog` (accel and brake on one
  joystick axis, accel the negative way, the brake a pedal, the steering a joystick axis) or
  `-Setup digital` (the same axes read as keys) switches the bindings before recording (and off,
  so `controls_update` leaves the scripted values alone; no joystick is read), `-Manual` gives the
  car a manual gearbox, and the script takes values as well as keys: `'150:accel=-700+steer=300,
  40:brake=600,10:accel+gearup'` (`accel=`, `brake=`, `steer=`, `right=`, `gearup`, `geardown`).
  `artifacts/traces/analog.trace`, `digital.trace` and `gears.trace` are the `physics.analog`,
  `physics.digital` and `physics.gears` tests, recorded with:

  ```powershell
  .\tools\dosbox\record-race.ps1 -Out artifacts\traces\analog.trace -Setup analog -Skip 200 -Frames 400 -Script '200:accel=-1023,80:accel=-1023,60:accel=-600+steer=300,40:accel=-800+steer=-700,30:accel=500+steer=150,40:brake=400,30:brake=-300+steer=-1023,50:accel=-1023+steer=1023,30:brake=1023+steer=-200,40:accel=-900'
  .\tools\dosbox\record-race.ps1 -Out artifacts\traces\digital.trace -Setup digital -Skip 200 -Frames 400 -Script '200:accel=-1023,80:accel=-1023,60:accel=-1023+steer=700,60:accel=-1023+steer=-300,40:brake=800,40:accel=-1023+steer=-1023,40:brake=300+steer=200,80:accel=-400+steer=450'
  .\tools\dosbox\record-race.ps1 -Out artifacts\traces\gears.trace -Manual -Skip 190 -Frames 420 -Script '190:accel,20:accel,5:accel+gearup,60:accel,5:accel+gearup,1:accel,5:accel+gearup,60:accel,3:accel+geardown,40:accel,10:accel+gearup+geardown,20:accel,4:geardown,37:brake,150:accel'
  ```
  They start after the countdown (`-Skip`) on the track the game's `CHOICE.DAT` had (MAP2 here;
  the default `-Autotype` takes the menu's last track), so the tests compare them with MAP2.
  

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
