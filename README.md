# Screamer HD

A native Windows version of **Screamer**, the 1995 DOS arcade racer by Graffiti / Virgin
Interactive, rebuilt from a full reverse engineering of the original program. It runs the
original game's data at any resolution, in widescreen, with gamepads and modern sound, and
drives exactly like the original. Car physics, collisions, CPU cars and input timing are
reproduced bit for bit, so the original's replay files play back identically.

> **No game data is included.** You need your own copy of Screamer (the DOS CD, or the GOG
> or Steam release). This is an unofficial fan project, not affiliated with or endorsed by the
> owners of Screamer. No licence is granted; see the [disclaimer](#disclaimer).

Version **1.1** · Windows 10/11 (64-bit) · built on [SDL3](https://libsdl.org)

---

## Contents

- [Features](#features)
- [System requirements](#system-requirements)
- [Installing](#installing)
- [Default controls](#default-controls)
- [The game](#the-game)
- [Systems in detail](#systems-in-detail)
  - [Simulation and timing](#simulation-and-timing)
  - [Renderer](#renderer)
  - [Display, resolution and widescreen](#display-resolution-and-widescreen)
  - [Sound and music](#sound-and-music)
  - [Controls and controllers](#controls-and-controllers)
  - [Replays, ghosts and records](#replays-ghosts-and-records)
  - [Menus and options](#menus-and-options)
  - [Saved files](#saved-files)
- [Command-line switches](#command-line-switches)
- [Untested and known limitations](#untested-and-known-limitations)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [FAQ](#faq)
- [Disclaimer](#disclaimer)
- [Credits and third-party licences](#credits-and-third-party-licences)

---

## Features

- **The whole game**: every track forwards and reversed (12), all cars including the secret
  one, single races, the championship with its four leagues and unlocks, Time Attack with
  ghosts, Cone Carnage and Slalom, the intro demo, records, replays, the cheats, the P
  "car stereo" panel and the announcer.
- **Identical driving**: physics, collisions, CPU cars and the original's 100 Hz frame clock
  are reproduced bit for bit and checked against recordings of the DOS game. Original `.RPL`
  replays play back exactly.
- **Any resolution**: from the original 320x200 up to 8K, or the window's own size, with the
  original's high-resolution (640x400) menus and HUD from 400 lines up.
- **Widescreen**: 16:10, 16:9, 21:9, 32:9 and anything in between. Races show more to the
  sides, the HUD keeps to the screen edges, and menus stay 4:3 with a blurred backdrop.
- **Smooth motion**: the game still runs at its original 50 steps a second, but pictures are
  drawn at your display's rate (60, 144, 240 Hz...), blended between steps.
- **Longer view distance**: 1x to 8x the original's draw distance, or no limit, fading out
  softly at the edge, so the designers' shortcuts and scenery show from far away.
- **Cleaner picture**: mipmapped textures (steady road lines in the distance), welded road
  joints, no cracks between road pieces, the sky follows the camera's roll, obstacles stand on
  the road, smooth turning at high resolutions. Each one can be switched back to the original
  look.
- **Fast multi-core software renderer** with SSE2 and AVX2 builds of its pixel filler.
- **Sound**: the original's sound effects and announcer, CD music played straight from the CD
  image or from the CD's tracks ripped to MP3/FLAC/OGG/WAV, or the original's MIDI music through an emulated OPL3 (Sound Blaster 16) chip and a
  port of HMI's own MIDI driver. Separate volumes for announcer, engine, effects and music.
- **Controllers**: gamepads, wheels, pedals and joysticks, hot-plugging, rumble, analog or
  digital axes, and a rebinding screen for keyboard and controller. Your old `CONTROLS.CFG`
  keys are imported.
- **Fullscreen or windowed**, vsync, frame limiter, all switchable live from OPTIONS > VIDEO.
- **Screenshots** (F12) and a **photo mode** with a free camera (F8).

## System requirements

| | Minimum | Notes |
|---|---|---|
| OS | **Windows 10 or 11, 64-bit** | Windows 7 SP1 and 8.1 (64-bit) should work (SDL3 and the runtime support them) but are untested; Windows 7 needs its Platform Update for Direct3D 11, or `--renderer opengl`. No Visual C++ redistributable is needed. |
| CPU | **Any x86-64 (64-bit) processor** | Every 64-bit x86 CPU has **SSE2**, which is all the game needs. |
| CPU extensions | **AVX2 optional** | With AVX2 (Intel Haswell / 2013 and later, AMD Excavator / Zen and later) the renderer's pixel filler processes 8 pixels at a time instead of 4, which makes high resolutions noticeably faster. The picture is exactly the same either way; AVX2 is detected at start-up. |
| Cores | 2 or more recommended | The 3D view is drawn on every CPU thread. The game logic itself is tiny. |
| Graphics | Any GPU with Direct3D 11, Vulkan or OpenGL | The 3D scene is drawn by the CPU; the GPU only scales and shows the finished picture. At 4K and above a fast CPU matters more than the GPU. |
| Sound | Any sound device | Optional (`--mute`). |
| Disk | About 10 MB for the program | Plus your Screamer install, and its CD image or ripped music tracks if you want CD music. |
| Game | **Your own copy of Screamer** | DOS CD (full install), GOG or Steam. |

As a rough guide, 1080p at 60+ fps works on any recent quad-core CPU; 4K at high refresh rates
wants a modern 6- to 8-core CPU with AVX2. Lower the resolution or view distance in
OPTIONS > VIDEO if it does not keep up. The driving is unaffected either way, because the
game always steps at its own fixed rate.

## Installing

1. **Download** `ScreamerHD-1.1-win64.zip` from the release page and unzip it anywhere, for
   example `C:\Games\ScreamerHD`.
2. **Copy your Screamer files** into the `SCREAMER` folder inside it, so that it looks like
   this:

   ```
   ScreamerHD\
     screamer.exe
     SDL3.dll
     opl3.dll
     SCREAMER\
       GFX\        <- from your Screamer install
       DATA\
       CONFIG\
       REPLAYS\    (optional: your old replays and records)
       SCREAM.cue  (optional: CD music, with SCREAM.bin)
       MUSIC\      (optional: CD music as MP3/FLAC/OGG/WAV files, see below)
       ...
   ```

   Where to find them:
   - **DOS CD**: the folder you installed the game to (usually `C:\SCREAMER`). Do a full
     install so that `GFX` and `DATA` are on the hard disk. For CD music, also make an image
     of the CD as `SCREAM.cue` + `SCREAM.bin` (cue/bin, raw 2352-byte sectors), or rip its
     music tracks to files in a `MUSIC` folder (next point).
   - **CD music as audio files**: if you have ripped the CD's music to MP3, FLAC, Ogg Vorbis or
     WAV files (with any CD ripper), put them in a folder called `MUSIC` inside the game folder,
     named by their CD track number:

     ```
     SCREAMER\
       MUSIC\
         02.flac  03.flac  04.flac  05.flac  06.flac  07.flac  08.flac
     ```

     The usual names rippers give work as they are: `02 - Title.mp3`, `Track02.flac`,
     `track02.cdda.wav`, `Screamer - 02 - Title.ogg`. The CD's track 1 is the game's data, so
     the music is tracks 2 to 8; a rip that numbered the music from 1 (`01` to `07`) is
     recognised and moved up by one. Any sample rate, mono or stereo. A track in the `MUSIC`
     folder is played instead of the same track of a CD image, so you can also replace single
     tracks; tracks it does not have still come from the image if there is one.
   - **GOG / Steam**: the game's install folder. Their CD image (`SCREAM.DAT` and
     `SCREAM.gog`) is used for the CD music automatically.

   Instead of copying you can also:
   - put `screamer.exe`, `SDL3.dll` and `opl3.dll` straight into the game's own folder, or
   - start `screamer.exe --data "D:\path\to\Screamer"`, or
   - set the environment variable `SCREAMER_DATA` to the game's folder.

   File and folder names are matched without regard to case.
3. **Run `screamer.exe`.** The intro demo plays, then the main menu. If the game files cannot
   be found, a message says where they go.

Nothing is written into the game's folder: settings, controls and replays go to
`%APPDATA%\ScreamerHD\` (see [Saved files](#saved-files)). Saves from test builds made before
the name Screamer HD (`%APPDATA%\ScreamerRecomp`) are copied over once. The first time, your original
`CONFIG\CHOICE.DAT` (settings, records, unlocked cars and tracks) and `CONFIG\CONTROLS.CFG`
(keys) are taken over from the game's folder.

To uninstall, delete the folder and `%APPDATA%\ScreamerHD`.

## Default controls

### Driving

| Action | Keyboard | Xbox-style gamepad |
|---|---|---|
| Accelerate | Up | Right trigger |
| Brake / reverse | Space or Down | Left trigger |
| Steer | Left / Right | Left stick or D-pad |
| Gear up / down (manual gearbox) | Page Up / Page Down | Y / X |
| Look back (hold) | B | Right shoulder (RB) |
| Car stereo panel (pause) | P | Start |
| End the race | Esc | Back / View |

All driving controls can be rebound in OPTIONS > CONTROLS.

### In a race

| Key | Action |
|---|---|
| F1 | Bumper view (full screen, compact gauges) |
| F2 | Cockpit view (the car's dashboard) |
| F3 | Chase view (default) |
| F4 | Far chase view |
| M | Radar: cycle off and three sizes |
| P | Car stereo panel: pauses the race; pick a CD/MIDI track, change volumes, EJECT switches between CD and MIDI music |
| F8 | Photo mode: freeze the race, hide the HUD and fly a free camera (see below) |
| F9 / Shift+F9 | Next / previous view distance |
| F10 | Mipmaps on/off |
| F12 | Save a screenshot to `Pictures\Screamer` |
| Esc | End the race (asks PLAY AGAIN?) / leave the panel / leave a replay |

Photo mode (F8): W/S forward and back, A/D sideways, E or Space up, Q or C down, arrow keys
to look around, Shift moves 5x faster, Ctrl 5x slower. F12 takes the picture; F8 or Esc goes
back to the race.

### Menus

The menus always answer to the arrows, Enter and Esc, and to the gamepad's D-pad / left stick,
A (confirm), B (back) and Start, whatever the driving bindings are. On the main page Esc or the
pad's Back quits the game. On a pad without a Back button, hold B for 2 seconds to quit (a ring
fills in the corner; tapping B does nothing there).

Typing letters in the menus enters the original's cheat codes.

## The game

Everything from the DOS game's menus works as it did:

- **Single race** on any unlocked track, forwards or reversed, over 3, 5, 10 or 25 laps,
  at the unlocked difficulty levels.
- **Championship**: four leagues of six races. A top-three finish goes on to the next race;
  winning a league unlocks tracks and the next league.
- **Time Attack**, against the clock and optionally your saved ghost.
- **Cone Carnage** and **Slalom**.
- **Records**: the five best laps and total times per track, with the replays of record
  races. Your original records come over from `CHOICE.DAT`.
- **Replays**: every race is recorded; an instant replay is offered after a single race.
- **Options**: detail (LOW / MID / HI), the original's options, plus new SOUND, VIDEO and
  CONTROLS pages.
- **Network game** shows a "not supported" notice; multiplayer is not supported yet.
- **Cheats** typed in the menus work as in the original (unlock the secret car, all tracks,
  other obstacles, stop the clock, mirror and upside-down worlds).

## Systems in detail

### Simulation and timing

The game logic is a line-by-line reconstruction of the original `STARTL.EXE`, with every
function and data structure named and documented in the source (`src/core/`). It keeps the
original's frame clock: a 100 Hz timer, game steps of at least 2 ticks (at most 50 steps a
second), the original's smoothing of the step length and its frame-rate damping table. The
x87 floating-point precision of the original is reproduced where it matters, so car physics,
collisions with walls, obstacles and CPU cars, the CPU drivers and the gearbox produce the
same numbers as the DOS game, bit for bit. This is verified by automated tests that replay
recordings of the original running in DOSBox and compare every frame.

Rendering and sound do not feed back into the simulation, so every graphics and sound option
is free to differ from the original without changing how the car drives.

### Renderer

The 3D view is drawn by a new software renderer, not a copy of the original's column fillers:

- **Perspective-correct textured polygons with a depth buffer**, in the original's 256-colour
  palette, at any resolution. The projection matches the original's at 320x200 and scales
  with the picture.
- **All the original's material types**: textured, textured with holes, flat, darkened
  (shadows), translucent (glass, water) and animated textures, moving glass reflections on
  the cars, tyre smoke, the crash tumble, wheels that steer, roll and lean.
- **Multi-core**: each picture is split into rows drawn on every CPU thread (`--threads`).
  The pixel filler is built twice, for SSE2 and for AVX2, and picks the best your CPU has
  (`--fill`). Both give exactly the same pixels.
- **Smooth motion**: the game steps 50 times a second; pictures in between are blended from
  the last two steps, so the game looks smooth on high-refresh displays without changing the
  simulation.
- **View distance** (`--view-distance`, F9, OPTIONS > VIDEO > DISTANCE): 1x, 1.5x, 2x
  (default), 2.5x, 3x, 3.5x, 4x, 5x, 6x, 7x, 8x the original's far clip, or MAX (no limit).
  The far edge fades out in an ordered dither instead of popping.
- **Full detail far away**: distant objects are drawn in full detail by default. FAR MODELS
  LOW (`--lod`) uses the original's simpler distant versions instead.
- **Mipmaps** (default on, F10, `--mipmaps`): textures seen squeezed in the distance are drawn
  from smaller averaged versions, halved separately along each axis so that the road's painted
  lines stay sharp and steady instead of breaking into dashes. Averaged in linear light and
  matched back to the palette. `--mip-sharpness` tunes it.
- **Fixes to the original's picture**, each switchable back:
  - welded road joints, so painted lines do not step at every joint (`--weld-joints`);
  - no hairline cracks between road pieces (`--seal-edges`, `--texture-edges`);
  - cones, barriers and signs standing on the road instead of half sunk into it
    (`--ground-obstacles`);
  - the sky tilts with the camera's roll (`--sky-tilt`);
  - camera, car and sky angles keep their fraction, so slow turns do not judder at high
    resolutions (`--smooth-turn`);
  - non-flat faces tested half by half, closing holes such as the cliff on track 6
    (`--folded-faces`).
- **HUD and menus**: the original's 320x200 graphics, or the 640x400 set of the original's
  high-resolution `STARTH.EXE` when the picture has 400 lines or more (`--ui`).

### Display, resolution and widescreen

The 3D scene is drawn at the **render size** (OPTIONS > VIDEO > RES, or `--render-size`) and
then scaled to the window by the graphics card through SDL3 (Direct3D 11 by default;
`--renderer` picks another). NATIVE draws at the window's own size. Pictures larger than the
window are averaged down in 2x2 steps (supersampling).

- **4:3** (default): the original's picture shape, letterboxed in any window.
- **WIDESCREEN ON** (`--aspect wide`): the picture takes the window's shape with square pixels.
  Races show more to the sides; the HUD keeps to the screen's edges; the menus stay 4:3 in the
  middle with a blurred copy of their backdrop in the side bars.
- Resolutions offered: NATIVE, the 4:3 sizes (including 320x200 and 640x400) and, with
  widescreen, 16:10, 16:9, 21:9 and 32:9 sizes up to 8K, each labelled with its shape.
- **Fullscreen** is borderless at the desktop's resolution.
- **Vsync** (default on) and a **frame limiter** (30, 60, 75, 120, 144, 165, 240 or off).

Every VIDEO setting applies immediately, without a restart.

### Sound and music

All sound is mixed in software at 44.1 kHz stereo and played through SDL3.

- **Sound effects** are the original's 8-bit 11 kHz samples, played with the original sound
  driver's volume, pitch, pan and looping: engine pitch from the revs, up to three CPU car
  engines with doppler and distance, skids, crashes, wall scrapes, track sounds (trucks,
  planes...). Resampling is smooth (linear) by default; `--audio-filter nearest` gives the
  original driver's crunchier sound.
- **The announcer**: countdown, wrong way, taking the lead, lapping, bends ahead and the rest.
- **CD music**: the original's soundtrack is CD audio. It is played straight from the CD image
  (`SCREAM.cue`/`.bin`, or `SCREAM.DAT`/`.gog` as GOG and Steam ship it), the same tracks the
  game picks for the menu, the intro and each track. Instead of an image, the CD's tracks can
  be MP3, FLAC, Ogg Vorbis or WAV files in a `MUSIC` folder in the game folder, named by CD
  track number (`02.flac` to `08.flac`, see [Installing](#installing)); a track found there is
  played in place of the image's. The menu plays track 4, the intro track 3, and the six
  tracks 3, 5, 6, 3, 5, 6.
- **MIDI music**: the original's MIDI versions (for players without the CD) from
  `DATA\MUSIC.DAT`, played on an emulated Yamaha OPL3 (the Sound Blaster 16's FM chip,
  [Nuked-OPL3](https://github.com/tgies/Nuked-OPL3-fast)) driven by a register-for-register
  port of HMI's original OPL3 MIDI driver, so it sounds as the DOS game did on a Sound Blaster.
- OPTIONS > SOUND: separate **ANNOUNCER, ENGINE (yours), EFFECTS and MUSIC** volumes (0-10),
  and **CD MUSIC / MIDI MUSIC**. Without a CD image or `MUSIC` folder the MIDI music plays
  automatically.
- The P panel in a race works in both modes: track buttons, the volumes, and EJECT switches
  between CD and MIDI music.

### Controls and controllers

- **Keyboard, gamepads and joysticks** (wheels, pedals, sticks) all work at once and can be
  plugged in and out at any time. Any controller drives, or OPTIONS > CONTROLS > PAD picks one.
- **Rebinding**: OPTIONS > CONTROLS > KEYBOARD and CONTROLLER list the seven driving actions
  (accelerate, brake, steer left, steer right, gear up, gear down, look back). Each takes two
  keys and two controller inputs: a button, a hat direction or an axis direction. Enter on a
  line waits for the new key or input (Esc or the pad's Back cancels); the new one becomes the
  first and the old first moves to second. The race's own keys (P, M, F1-F12) cannot be taken.
- **Analog or digital axes**: on the CONTROLLER page, Left/Right on an action switches its
  axes between analog and digital (read as a key, like the original's "joystick as keys").
  Pedals that rest at one end of their travel use the whole travel.
- **RUMBLE ON/OFF**: vibration follows what the car does (crashes, wall hits, skids, bumpy
  ground). It never feeds back into the driving. Applies at once.
- **DEFAULTS** puts all bindings back.
- **Your old keys**: the first time, the keys set in the original's SETUP.EXE
  (`CONFIG\CONTROLS.CFG`) become each action's first key, with the defaults second.
- **Identical handling**: whatever is bound, the game receives its inputs exactly as the
  original's control code produced them (keys at full value with the original's ramps; axes
  through its clamp and dead zone; a brake pedal set directly; the stick as the steering
  angle), so the car handles as in the original and replays stay exact.

### Replays, ghosts and records

- Every race is recorded in the original's `.RPL` format (the inputs of every frame) and
  played back by running the race again, so replays are tiny and exact.
- After a single race, an **instant replay** is offered. Record-breaking races keep their
  replay, viewable from the records page ("R"). Replays use the original's TV-style trackside
  cameras.
- Original replays from the DOS game's `REPLAYS` folder play back too, as do its Time Attack
  ghosts (`MAPn.ATK`).
- `--replay FILE.RPL --track MAPn` plays a replay file from the command line.

### Menus and options

The menus are the original's 3D menus, rebuilt. New pages:

- **OPTIONS > SOUND**: ANNOUNCER, ENGINE, EFFECTS, MUSIC volumes; CD MUSIC / MIDI MUSIC.
- **OPTIONS > VIDEO**: FULLSCREEN, WIDESCREEN, RES, DISTANCE, DETAIL; ADVANCED: VSYNC,
  FPS LIMIT, FAR MODELS (FULL/LOW), MIPMAPS.
- **OPTIONS > CONTROLS**: KEYBOARD, CONTROLLER, RUMBLE, PAD, DEFAULTS.

DETAIL works as in the original: LOW loads the low-detail track and has no moving scenery;
MID and HI load the high-detail track.

### Saved files

Everything is kept in `%APPDATA%\ScreamerHD\`:

| File | Contents |
|---|---|
| `CHOICE.DAT` | The original's settings file (car, track, name, records, unlocks, options), with our SOUND and VIDEO settings appended. The DOS game can still read the original part. |
| `CONTROLS.INI` | Key and controller bindings, rumble, chosen pad (plain text). |
| `REPLAYS\` | Replays of record races (`REPLAY<track><n>.RPL`). |
| `MAPn.ATK` | Time Attack ghosts, in the original's format. |

Screenshots (F12) go to `Pictures\Screamer\`. Command-line options override the saved settings
for that run only.

## Command-line switches

`screamer.exe --help` lists them all. Options given on the command line win over the saved
settings for that run.

### Everyday

| Switch | Meaning |
|---|---|
| `--data DIR` | The original game's folder (default: `SCREAMER_DATA`, then a `SCREAMER` folder next to the program, then the program's own folder). |
| `--fullscreen` / `--windowed` | Borderless fullscreen or a window (default: as OPTIONS > VIDEO). |
| `--size WxH` | Window size. |
| `--render-size WxH` | Internal resolution of the 3D view (e.g. `1920x1080`). |
| `--aspect MODE` | `4:3` (default), `wide` (widescreen), `stretch` or `square`. |
| `--ui low\|high\|auto` | HUD and menu graphics: 320x200, 640x400, or auto (640x400 from 400 lines). |
| `--vsync on\|off` | Wait for the display's refresh (default on). |
| `--frame-limit N` | At most N pictures a second (0: no limit). |
| `--view-distance X` | Times the original's draw distance (1-8; 0: no limit; default 2). |
| `--detail high\|low` | High- or low-detail track files. |
| `--level N` | Opponent level 0-4 (default 1). |
| `--renderer NAME` | SDL renderer: `direct3d11`, `direct3d12`, `vulkan`, `opengl`, `software`, `gpu`. |
| `--threads N` | Threads drawing the 3D view (default: all CPU threads). |
| `--mute` | No sound at all. |
| `--volumes A,E,F,M` | Announcer, engine, effects, music volumes 0-10. |
| `--sfx-volume N`, `--music-volume N` | Overall effects / CD music volume 0-100. |
| `--no-music`, `--no-speech` | No music / no announcer. |
| `--music-source cd\|midi\|auto` | CD audio (image or `MUSIC` folder), MIDI on the OPL3, or CD when there is CD audio. |
| `--audio-filter linear\|nearest` | Smooth or original-style resampling of the effects. |
| `--replay FILE` | Play back a `.RPL` replay (with `--track` for its track). |
| `--save-dir DIR` | Keep settings and replays in DIR instead of `%APPDATA%`. |
| `--version`, `--help` | Version / list of options. |

### Picture tweaks (each restores an original behaviour or tunes ours)

| Switch | Meaning |
|---|---|
| `--mipmaps on\|off` | Mipmapped textures (default on). |
| `--mip-sharpness X` | 1 sharper ... 2 softer (default 1.4). |
| `--lod` | The original's simpler models in the distance (default: full detail). |
| `--weld-joints off` | Road pieces exactly where the track file puts them. |
| `--seal-edges all` | Road pieces extend past every face edge (older method). |
| `--texture-edges wrap` | Texture coordinates wrap at polygon edges (older method). |
| `--ground-obstacles off` | Obstacles half below the road, as the original. |
| `--sky-tilt off` | The sky stays level when the camera rolls. |
| `--smooth-turn off` | Angles in whole 2048ths of a turn, as the original. |
| `--folded-faces off` | Non-flat faces judged by one normal, as the original (shows its holes). |
| `--face-lists off` | Draw every front-facing face, not only those in the draw order. |
| `--fill sse2\|avx2` | Force a build of the pixel filler (same picture). |
| `--upload index\|rgba` | How the picture is passed to the graphics card. |

### Developer and testing

| Switch | Meaning |
|---|---|
| `--headless` | No window, software renderer, no audio device; deterministic (each frame is two ticks of the 100 Hz clock). |
| `--hidden` | Like `--headless` but on the real graphics driver, with a window that is never shown. |
| `--frames N` | Quit after N game frames. |
| `--screenshot FILE` | Save the final picture (`.png`/`.bmp`). |
| `--dump-fb FILE` | Save the final internal framebuffer. |
| `--capture-every N`, `--capture-from N`, `--capture-dir DIR` | Save the framebuffer every N frames. |
| `--record-audio FILE` | Write the sound of the run to a 44.1 kHz `.wav` (works headless). |
| `--scene NAME` | Start in `menu`, `title`, `track`, `drive` or `testpattern` instead of the intro demo. |
| `--menu-keys LIST` | Scripted menu keys, e.g. `40:down,60:enter`. |
| `--track MAPn`, `--reversed` | Track for `track`/`drive` scenes and replays (MAP1-MAP6). |
| `--car-index N`, `--view N`, `--start X,Z`, `--game-mode N`, `--race-options N` | Race setup for `--scene drive`. |
| `--hold KEYS` | Controls held every frame in `--scene drive` (e.g. `accel,left`). |
| `--record FILE` | Save the `--scene drive` race as a `.RPL`. |
| `--positions FILE` | Write every car's position every frame. |
| `--camera X,Y,Z,YAW,PITCH[,ROLL]`, `--car NAME[,...]`, `--flat` | Camera, cars and untextured mode for `--scene track`. |
| `--perf` | Print render timings every 5 seconds. |
| `--tween-test`, `--audio-log` | Test the in-between pictures; log every sound. |

## Untested and known limitations

- **Controllers other than Xbox-style pads are untested.** Only Xbox-layout gamepads have
  been tried. PlayStation, Switch Pro and other pads that SDL3 recognises should work with the
  same layout (A = bottom face button, B = right, X = left, Y = top), and wheels, pedals and
  flight sticks should work through rebinding, but none of these has been tested. Rumble has
  only been tried on an Xbox pad. Reports are welcome.
- **Windows 7, 8 and 8.1** should work but are untested.
- **Multiplayer** (IPX network and serial link) is not supported; NETWORK GAME shows a notice.
- **Only Windows** builds are provided. The code uses SDL3 throughout, but other platforms have
  not been built or tested.
- **High refresh rates at very high resolutions** depend on the CPU, since the 3D scene is
  drawn in software. A GPU renderer is planned for a later version.
- Some odd-looking things on tracks (seams, small gaps in the scenery) are in the original
  track data and are drawn as the original does.
- The game files have only been tested with the DOS CD version; the GOG/Steam layout (with
  `SCREAM.DAT`/`SCREAM.gog` for the music) is supported from their known file names.

## Troubleshooting

- **"Game files not found"**: the folder given (or `SCREAMER` next to `screamer.exe`) must
  contain `GFX` and `DATA` directly, not in a subfolder.
- **No music**: the CD music needs the CD image next to the game files, or the ripped tracks
  in a `MUSIC` folder there, named by CD track number (`02` to `08`). The log (see below) says
  how many ripped tracks were found. Switch OPTIONS > SOUND to MIDI MUSIC to use the built-in
  MIDI music instead.
- **Slow at high resolutions**: lower RES or DISTANCE in OPTIONS > VIDEO, or turn widescreen
  off. Check that the game uses AVX2 (`screamer.exe --perf` from a command prompt prints
  timings).
- **Black screen or display trouble**: try `--renderer opengl` or `--renderer vulkan`, or
  `--windowed`.
- **Start over**: delete `%APPDATA%\ScreamerHD\CHOICE.DAT` (settings) and/or
  `CONTROLS.INI` (bindings).
- **Log output**: start `screamer.exe` from a command prompt; it prints its log there.

## Building from source

Requires Windows, Visual Studio 2022 (or its Build Tools) with the C++ workload. CMake, Ninja,
SDL3 and Nuked-OPL3 are fetched or found automatically.

```powershell
.\tools\build.ps1                    # debug build
.\tools\build.ps1 -Preset release    # optimised build
.\tools\test.ps1                     # build and run the tests (headless)
.\tools\run.ps1                      # play
.\tools\package.ps1                  # the release zip (artifacts\release)
```

For development, put your game files in `SCREAMER\` at the repository root (it is ignored by
git) or set `SCREAMER_DATA`. Tests that need the game data skip themselves without it.

See [docs/HARNESS.md](docs/HARNESS.md) for the test harness (headless runs, golden images,
physics comparisons against the original in DOSBox) and
[docs/REVERSING.md](docs/REVERSING.md) for how the original program was reverse engineered.

```
src/core/        the game: simulation, renderer, sound, menus (no window code)
src/main.cpp     SDL3 frontend: window, display, audio device, main loop
src/input.cpp    keyboard, gamepads, joysticks, bindings
src/tools/       image and physics-trace comparison tools
tests/           unit, render, smoke and physics tests
tools/           build, test, run and packaging scripts; DOSBox and Ghidra harness
reference/       symbol map and hashes of the original (never the original files)
docs/            documentation
```

## FAQ

**Q: Why are there still visible seams and gaps?**

A: Graffiti didn't always line things up precisely, and they often cut out
polygons entirely when they thought they were insignificant to the final presentation. These 
cracks are also present in the original game, but at the 320x200 and 640x400 resolutions 
available they were essentially invisible. I've closed the gaps in the roads and made sure the 
lines on the roads are contiguous as much as possible, but fixing all of it would require 
modifying the track models themselves.

**Q: Why do the car wheels have no upper portions?**

A: Likely because Graffiti were trying to keep the fill rate as low as possible to keep 
performance up on 1995 hardware. They were pretty ruthless with their trimming. It also 
prevented the upper parts of the wheels from clipping through the car body when the car 
leans. Just another shortcut, basically.

**Q: Why when using extended draw distances are buildings and other track objects seemingly 
appearing incomplete?**

A: Again, fill rate. At the native (very short) draw distance those would never be drawn so the 
player wouldn't see them, and the low resolutions helped disguise any visual anomalies. I 
recommend using 1.5x to 2x view distance to minimise the undesirable effects while maintaining a 
reasonable range of vision, but you can go nuts if you want.

**Q: Why are there lots of holes in the geometry off the track?**

A: I'm sure you get it by now but: fill rate. A pixel not drawn is time saved. This is just 
how the game was built. Fixing it would require remodelling the tracks.

**Q: Why only a software renderer?**

A: I was intrigued by how far performance could be pushed with a software rendering solution.
The answer is: pretty far on modern hardware! I also wanted to focus on it being stable and 
feature-complete before moving onto changing its visuals significantly. Having said that, the 
next version is planned to ship with a hardware renderer.

**Q: Was the original software renderer that bad?**

A: Absolutely not. It was a highly optimised piece of x86 assembly, about as good as you could
write for machines at the time. Graffiti did a great job with it, but time has moved on.

## Disclaimer

Screamer HD is an **unofficial fan project**. It is not affiliated with, authorised or
endorsed by Graffiti, Virgin Interactive, their successors, HMI (Human Machine Interfaces) or
any other rights holder.

- **Screamer**, its name, program and data (graphics, tracks, cars, sound, music, text) are
  the property of their owners: Graffiti / Virgin Interactive and their successors. The MIDI
  music driver reproduced here is the work of HMI and remains theirs.
- **No game data is included** in this repository or in the release downloads. You need your
  own legally obtained copy of Screamer to play.
- Parts of this program are a reconstruction of the original program's behaviour, made for
  preservation and so that owners of the game can play it on modern systems.
- **No licence is granted.** All rights are reserved; this code is not offered under an open
  source licence. The third-party components below keep their own licences.
- The software is provided **as is**, without warranty of any kind. Use it at your own risk.

If you are a rights holder and have concerns, please contact the repository owner.

## Credits and third-party licences

- **Screamer** (1995) was developed by Graffiti and published by Virgin Interactive.
- [SDL3](https://github.com/libsdl-org/SDL), zlib licence: window, graphics, audio and input.
  Licence in `LICENSES\SDL3.txt`.
- [Nuked-OPL3-fast](https://github.com/tgies/Nuked-OPL3-fast) by Nuke.YKT and contributors,
  GNU LGPL 2.1: the OPL3 emulation, shipped as its own `opl3.dll`, which you may replace with
  your own build. Its licence and the exact source used are in `LICENSES\` and
  `third_party\Nuked-OPL3-fast\` in the release.
- [dr_libs](https://github.com/mackron/dr_libs) (dr_mp3, dr_flac, dr_wav) by David Reid, public
  domain or MIT-0: MP3, FLAC and WAV decoding for the `MUSIC` folder. Licence in
  `LICENSES\dr_libs.txt`.
- [stb_vorbis](https://github.com/nothings/stb) by Sean Barrett, public domain or MIT: Ogg
  Vorbis decoding for the `MUSIC` folder. Licence in `LICENSES\stb_vorbis.txt`.
