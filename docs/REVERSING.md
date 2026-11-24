# Reverse engineering the original executable

This is how we analyse `STARTL.EXE` in Ghidra and keep the results in git without
committing anything derived from the game itself.

## The executables

| File | What it is |
|---|---|
| `STARTL.EXE` | The game, low-resolution build (320x200 VGA). 1,034,919 bytes. **Analysed here.** |
| `STARTH.EXE` | The same game, high-resolution build. 1,034,923 bytes; same layout, code differs slightly. |
| `SETUP.EXE` | Sound card setup, writes `config\setup.cfg`. |
| `DOS4GW.EXE` | Rational DOS/4GW 32-bit extender (the stub in the EXEs loads it). |
| `HMI*.386` | HMI SOS sound drivers loaded at run time (`hmidrv.386`, `hmimdrv.386`, `hmidet.386`). |

Both `START*.EXE` are MZ stubs (`WSTUB`) followed by a **Linear Executable (LE)** built with
**Watcom C/C++ 10.x** (the C runtime banner is "WATCOM C Run-Time system ... 1988-1993",
the stub reports "RATIONAL DOS/4G"). There is no debug information. The LE has three objects:

| Object | Preferred base | Size | Contents |
|---|---|---|---|
| 1 `code1` | `0x00010000` | `0xDB7A3` | Code **and** large read/write tables (the 3D engine keeps its data and self-modifying code here) |
| 2 `data2` | `0x000F0000` | `0x82` | Zero-filled: the keyboard state array (`g_key_state`) |
| 3 `data3` | `0x00100000` | `0x472C0` | DGROUP: initialised data (first 0x7000 bytes), then BSS and the stack |

Entry point `0x2FEB8` (`_cstart_`), initial ESP `0x1472C0`. 11,249 internal fixups, no imports.

## Pipeline

Ghidra 12 has no LE loader, so `tools/le/le2elf.py` converts the LE into a static ELF32:
every object is placed at its preferred base, all fixups are applied (so pointers hold final
addresses), and the zero-filled tail of each object becomes a NOBITS section. The fixup list
is written next to it as `<exe>.fixups.json`.

```powershell
.\tools\ghidra\apply-symbols.ps1 -Fresh -Dump     # convert, import, analyse, apply symbols, dump
.\tools\ghidra\apply-symbols.ps1 -Dump            # re-apply reference\symbols\STARTL.sym and re-dump
.\tools\ghidra\apply-symbols.ps1 -Exe STARTH -Fresh
```

`-Fresh` runs the full Ghidra auto-analysis (about a minute) and then
`tools/ghidra/scripts/ScreamerInit.java`, which

* installs a `__watcall` calling convention (Watcom register convention: arguments in
  EAX, EDX, EBX, ECX, then the stack; callee pops; every register except the return
  registers is preserved) as a compiler-spec extension, and
* marks each 32-bit fixup that is not inside an instruction as a pointer.

Every run then applies `reference/symbols/<Exe>.types.h` and `reference/symbols/<Exe>.sym`
with `ApplySymbols.java` (types, names, calling conventions, prototypes, comments) and, with `-Dump`, writes these to
`artifacts/ghidra/<Exe>/dump/`:

| File | Contents |
|---|---|
| `functions.tsv` | address, name, size, number of callers/callees, `INT`s used, I/O ports, strings referenced |
| `calls.tsv` | call graph edges |
| `disasm.txt` | disassembly per function with resolved references and strings |
| `decomp.c` | decompiled C for every function |
| `coverage.txt` | instruction/data/undefined bytes per block and large undefined gaps |

The Ghidra project itself (`artifacts/ghidra/<Exe>/proj`) can be opened in the Ghidra GUI.
Names given in the GUI are not kept: add them to the `.sym` file, which is the source of truth.

## Data types

`reference/symbols/STARTL.types.h` holds the recovered records as C structs. `ApplySymbols.java`
parses it with Ghidra's C parser before applying the symbol map, so `type=` and `sig=` can use
the types (for example `g_player_car` is a `PlayerCar` and the car functions take a `PlayerCar *`).

* Fields named after their meaning are confirmed (from the code, the data files or the running game).
  Every field the code touches in `PlayerCar`, `CpuCar` and `NetCar` has a name; a few names that
  only describe how a field is used (`frame_flags_010`, `engine_f178`, `tv_cam_a`...) are worth
  revisiting when that code is ported.
* `f_<offset>` fields have a known type, from how the code accesses them, but no known meaning yet.
  `pad_<offset>` are bytes that no analysed function touches.
* `tools/ghidra/check_types.py` checks every struct against the size in its comment; run it
  after editing the header.
* `tools/ghidra/field_usage.py` lists every access through a function's first parameter in the
  decompiler dump, or drafts a struct (`--struct Name --size N`) to start a new record from.

Much of the game's configuration comes from text `.DTR` files inside the `.DAT` archives, read by
`data_load_section`. Each section starts with a header comment that names the fields, and each
value is prefixed with its type (`s` = `char[12]`, `d` = int, `x` = hex int, `f` = float), so
the record structs (`CarData`, `OpponentDef`, `TrackData`, `ObstacleDef`, `AnimationDef`,
`SoundFxDef`) use the developers' own field names. `tools/data/datfile.py` lists and extracts
archive entries, including the LZW-compressed ones:

```powershell
py -3.14 tools\data\datfile.py list SCREAMER\DATA\GEN.DAT
py -3.14 tools\data\datfile.py cat SCREAMER\DATA\GEN.DAT CARS.DTR
```

(Extracted data stays out of git, like the game itself.)

## Symbol map format

```
<hexaddr> f <name> [cc=<convention>] [sig=<C prototype>] [; comment]
<hexaddr> d <name> [type=<data type>] [; comment]
<hexaddr> l <name> [; comment]
```

* Default calling convention is `__watcall`; varargs functions (`printf`, `sprintf`,
  `fprintf`, `sscanf`, `fscanf`) are `__cdecl`.
* `sig=` must be last on the line (it runs to the `;`).
* A name ending in `?` is a guess: Ghidra gets the name without the `?` and an
  "(unconfirmed)" comment. Remove the `?` once the function is confirmed.
* Library functions use their Watcom C / HMI SOS API names; game code uses snake_case
  grouped by subsystem prefix (`obj_`, `MemMgr_`, `archive_`, `cd_`, `sfx_`, `race_`, ...).

## Game code coverage

All functions in the game's own code (`0x10000`-`0x2FE10`, `0x4BCA0`-`0x4D0E4`) now have a
name; the remaining unnamed functions are in the libraries (HMI SOS internals, x87 emulator,
runtime helpers). Some findings worth knowing for the port:

* **Replays re-simulate the race.** `replay_write_frame` stores only the frame time and the
  driving inputs (0x14 bytes per frame); `replay_read_frame` feeds them back. The port must
  keep the simulation bit-for-bit deterministic (same float maths and frame times) for old
  replays to play back.
* **Timing:** `sound_init` registers `timer_tick_callback` with the HMI timer at 100 Hz.
  `frame_timing_update` waits for at least 2 ticks (so at most 50 fps), takes
  `ticks * 0.01 s` as the raw frame time, smooths it as `(9 * old + raw) / 10` and clamps it to
  0.02-0.2 s. The simulation therefore uses a variable step quantised to 10 ms, which the
  replay records per frame.
* **Damping table:** `build_pow_table` fills 50 rows x 100 floats with
  `pow(0.5 + 0.005 k, 50 / (row + 1))`, and each frame uses row `trunc(1 / frame_time)`.
  That row is one higher than "frames per second - 1", and when the frame time settles at
  exactly 0.02 s it is row 50, past the end of the table. In the running game the smoothing
  usually stops a few ulps above 0.02 (0.020000007) and uses row 49. `src/core/sim_clock.*`
  reproduces the clock and the table bit for bit (verified against the game's memory).
* **Time-attack ghosts** store positions instead (`ghost_record_frame`, 0x28 bytes per frame).
* Frame-rate independence comes from `g_frame_pow_table`: pow(k, frame_time) factors used by
  the `*_lerp_toward` helpers and the damping of speeds and yaw rate.
* Car update order (`race_update_vs_cpu`): `car_integrate_motion`, CPU cars
  `opponent_update_position`, `car_update_driving`, `opponent_update_driving`,
  `car_update_dynamics`, `opponent_update_state`, `car_update_track_contact`,
  `opponent_update_track_contact`, collisions, `opponent_update_contacts`, `camera_update`,
  `car_sync_render_object`, `opponent_sync_render_object`.

## Renderer

The 3D renderer is hand-written assembly (`0x81000`-`0x8C000` for the pipeline,
`0xCA6E0`-`0xDC8F0` for the polygon fillers) that patches its own code at run time (view
height, texture base addresses). Much of it is only reached through the material dispatch table,
so `ScreamerInit.java` turns orphan code into functions and only marks fixups as pointers inside
code when they form tables (a lone fixup there is usually an instruction operand).

* Fixed point: matrices and `g_sincos_table` are 1.15/2.14; angles run 0..0x7FF (2048 per turn).
* Per frame (`render_race_scene`, and `render_menu_scene` for the menus): camera matrices (`camera_build_matrices`),
  background band, collect and depth-sort the visible map cells (`vis_*`), then for every cell
  and object `object_begin` -> `object_transform_vertices` -> `object_build_polygons` ->
  `draw_polygon_list`.
* `g_material_handlers` (0x822E0) has four handlers for each of 16 material types:
  `draw`, `draw_alt`, `build_polygon`, `build_polygon_alt`. `build_polygon` projects a face,
  clips it at the near plane and adds it to `g_poly_buffer`; `draw_polygon_list` then calls
  `draw` or `draw_alt` (bit 2 of the entry) per polygon. The textured fillers walk vertical
  columns (destination += 320), which matters for any higher-resolution port: the column loop,
  the patched pitch (`raster_patch_pitch_*`) and `g_row_offsets` (481 rows) assume a
  320-byte-wide buffer.
* Running original (intro): view 320x200, `g_material_handlers` relocated by the code delta,
  222 visible cells, 84 drawn in depth order.

* The track and object file formats (`.UNR`, `.MMP`, `.MLB`, `.COL`, `.BCK`, `.SMG`, `.3DX`)
  are documented in `src/core/track.h` and `src/core/model3d.h`; `src/core/render3d.cpp`
  reproduces the camera and projection (`PERSP01.TAB`: screen = centre - 128 * x / (128 - z)).
  Checked against frozen frames of the intro demo (`capture-vga.ps1 -Freeze`):
  * the game loads `GFX/MAPnH.DAT` (high detail), not `MAPnL.DAT`;
  * the projection centre is (160, 130) at 320x200 (`g_render_camera` +0x2c/+0x30), and the
    background's horizon row is that centre + 128 * sin(pitch); the sky then matches exactly;
  * objects turn by -yaw relative to the camera's convention;
  * material types (`g_material_handlers`): 0-2 textured, 3-7 textured with texel 0
    transparent, 8-9 flat (face +0x02 colour), 10-11 textured (span filler), 12-13 darken
    through `build_darken_tables`' x0.6 remap, 14-15 translucent through `MAPn.SMG`
    (new = smg[texel * 256 + old]);
  * material frames: cell_begin sets the phase from the instance's animation word (>> 2) or,
    for -1, from the global counter `0x121538` (0.3 turns per second, `frame_timing_update`);
    a material shows frame `frames * phase >> 14`. Car materials have 8 frames: 0x8000 picks
    the second paint scheme;
  * object depth (`object_compute_depth`): distance of the bounding centre, with a bias for
    names not starting with 'A'/'a'; it also picks one of three entries of the object by
    distance (probably detail levels, not used here);
  * no distance fog anywhere in the renderer.
* Float code: Ghidra drops most x87 math in this binary (values kept on the FPU stack across
  `__CHP`, `sin`, `cos`). `tools/ghidra/x87_lift.py FUNC_ADDR` follows the FPU stack through
  the dump's disassembly and prints each store, compare and conversion as an expression, with
  `PlayerCar` fields named and constants read from the executable. Read it next to the
  decompiler output (which keeps the integer logic and control flow).
* Player car (`src/core/car.cpp`): race_frame runs car_integrate_motion, car_update_driving
  (engine/gearbox, throttle, brake, steering), car_update_dynamics (wheel loads, skid) and
  car_update_track_contact (corner probes, wheel ground heights, track heading, body pose)
  in that order. Walls are wherever the corner probes find no road face; the push comes from
  the nearest slot 3 cross-section. Smoothing goes through `lerp_toward` with the per-fps
  `g_frame_pow_table`. Checked frame by frame against recordings of the original (tracecmp):
  acceleration, the start wheelspin (crash_state 5), steering, braking, wall hits and high-speed
  cornering match exactly. The ground test is the game's own (integer edge functions in the
  placement's frame, retried 10 and 20 units along +x/+z); heights on slopes can still be off by
  one because the reference point of collision_test_object_faces' plane formula is unclear
  (at run time it reads the part header). Spins, CPU cars, checkpoints and sounds are not
  ported yet.
* CPU cars (`src/core/opponent.cpp`): each follows its own racing line (MAPnOPP.SPL, a quadratic
  B-spline walked by arc length), with a sideways offset from bumps that decays back to the
  line; speed heads for OPPONENTS.max_spd (+ skill while behind the player) times the speed
  factor of the current track marker (MAPn.MRK). race_frame interleaves them with the player:
  positions, player, driving, angles/crashes, ground and markers, then contacts. Cars further
  than 35000 units from the listener (the player) are frozen. Race positions only change by
  swapping the player with one CPU car per frame. Checked frame by frame against recordings of
  the original (start, mid-race with a bump from the player, far from the player): exact.
* Race HUD (`src/core/hud.cpp`): hud_draw / hud_draw_cockpit / hud_draw_for_view with the
  GAMEL.FNT fonts (4-bit glyphs, colour = pixel | mode << 4), GAMEL.SPR sprites and the
  run-length PKC overlays (IMPLET_L labels, IMPRES_L gauges); formats in `src/core/hud.h`.
  The radar draws a line per track cross-section of the 17 x 17 cells around the player and a
  dot per CPU car; the race starts after race_countdown_update's 3-2-1 lights (two seconds
  each). Checkpoints (marker +0x10 bit 1) start the clock and add time_left / 5 + bonus_time;
  a lap counts when marker 0 is reached with every marker passed.
* Sound (`src/core/sound.cpp`, `race_audio.cpp`, `cd_audio.cpp`): HMI SOS DIGI at 11025 Hz
  (`sound_init`), samples are unsigned 8-bit `.RAW` in DATA/SOUND.DAT, described by SOUND_FX
  records (SFXSBASE, the track's MAPnDAT.DTR, then the announcer's SPFXBASE/SPFXFULL; the game
  plays them by index). Engine pitch is `rpm * 14 + 32768` (16.16); the CPU engines (3 voices
  for the nearest cars) use `(last distance - distance) * 9 + speed * 2.4 + 13568` for a
  doppler shift, volume `((35000 - d) / 35000)^2 * 23552` and pan
  `0x8000 +- sqrt(|local x / d|) * 32767` from the player's car. The announcer is a queue of
  events (table at 0x101fb0, 27 events x 3 samples), played 0.7 s apart; speech_check_events
  raises the in-race ones. Music is CD audio (MSCDEX): menu track 4, races `table[map + 2] + 1`
  from 0x101efc; MUSIC.DAT holds the same tunes as HMI MIDI (`MUSICn.HMP`, n = CD track - 1)
  with AdLib FM banks (MELODIC.BNK, DRUM.BNK), played by `src/core/fm_synth.cpp` (formats in
  its header). Without SPFXFULL only the first sample of each announcer event exists.
* Moving scenery (`src/core/animations.cpp`): ANIMATIONS in MAPnDAT.DTR (object, speed, spline
  in MAPnANM.SPL, sound) move along racing-line style splines whose knots carry a height at
  +0xc; within 52000 units of the player they advance speed * frame time, heading from acos of
  the direction. Their looping sounds play within 40000 units with a doppler pitch
  `(last d - d) * 9 + 65536`, volume `((40000 - d) / 40000)^2 * 32767` (sfx_update_positional).
* Ground and track lines: `collision_query_cells` tests the faces of slot 0 (road) objects in
  the 3x3 cells around a probe, and the 4th letter of the part name is the surface type.
  `track_find_nearest_point` uses slot 2 (centre-line points with heading) and slot 3 (road
  cross-sections: centre, left and right edge).

## Checking against the running original

`tools/ghidra/runtime-probe.ps1` runs the original in DOSBox headless (SDL offscreen driver, no
window; `run-original.ps1 -Headless`), dumps emulated memory after a
delay (optionally after `-Autotype` input), finds where DOS/4GW actually loaded the objects
and prints every `d` symbol from the map at its runtime address:

```powershell
.\tools\ghidra\runtime-probe.ps1 -Delay 10                                   # intro demo race
.\tools\ghidra\runtime-probe.ps1 -Delay 9 -Autotype '-w 4 esc' -Only g_game_mode
```

Under DOSBox with the harness config, DOS/4GW loads `code1` at `0x1D2000` (delta
`+0x1C2000`) and `data3` at `0x2AE000` (delta `+0x1AE000`). During the intro the probe shows
`g_game_mode = 0x100`, `g_track_name = "MAP2"`, `g_num_opponents = 9`, `g_game_dir = "C:"`
and the video buffers in conventional memory, which is what `attract_mode_setup`,
`read_screamer_cfg` and `MemMgr_alloc` imply.

## What is known so far

See `reference/symbols/STARTL.sym` for every name; 928 of the 1,118 functions are
named. Address ranges in `code1`:

| Range | Contents |
|---|---|
| `0x10000`-`0x2FE10` | Game code (C): main loop, 3D object manager, race/car logic, menus, config and save files, CD audio, MIDI music |
| `0x2FE10`-`0x30200` | Watcom runtime: `operator new/delete`, `printf`, `exit`, `_cstart_` |
| `0x30200`-`0x4B000` | Tables (track geometry buffers, lookup tables) |
| `0x4BCA0`-`0x4D0E4` | Game code: data archives (`*.DAT`, LZW), 3D engine setup, PCX loader |
| `0x4D0E4`-`0x4D850` | Runtime: `sprintf`, x87 helpers (`__CHP`, `sin`, `cos`) |
| `0x81000`-`0x8C000` | 3D renderer (hand-written assembly, self-modifying), VGA palette/blit |
| `0xC1D00`-`0xCA700` | Runtime math/stdio, HMI SOS DIGI and TIMER API, IPX network |
| `0xC1FC0`-`0xDC8D0` | More renderer code and tables |
| `0xD1A00`-`0xE6000` | Runtime (heap, stdio, DOS I/O) and HMI SOS internals/driver loader |
| `0xE6000`-`0xEB7A3` | Runtime: x87 emulator, number formatting, time functions |

Key facts for the port:

* **Startup**: `main` creates a memory manager (`MemMgr_*`, a C++ class that hands out DOS
  or DPMI memory by name), refuses to run with less than 6,400,000 bytes free, then
  `game_init` reads `screamer.cfg`, `config\setup.cfg`, `config\controls.cfg`, initialises
  sound (`sound_init`), CD audio (MSCDEX, `cd_init`) and MIDI (`MidiMusic_init`), the
  keyboard handler (`keyboard_install`), the 3D engine (`engine_init`) and VGA.
* **Main loop**: `attract_mode_run` (the intro demo race), then `frontend_run` (menus);
  the menus start races with `frontend_start_race` -> `race_run` -> `race_loop` -> `race_frame`, which dispatches on
  `g_game_mode` to `race_update_*`, renders with `render_set_camera` + `render_race_scene`,
  draws the HUD and presents with `present_frame` (64,000-byte copy to `A000:0`).
* **Timing**: `frame_timing_update` waits until the timer interrupt has counted at least two
  ticks and derives a smoothed frame time (`g_frame_time`, clamped to 0.02-0.2 s).
* **Input**: INT 9 keyboard handler fills a 128-byte scancode table; `keyboard_snapshot`
  copies it into `g_key_state` (object 2) once per frame; `controls_update` maps bindings
  from `config\controls.cfg`; joystick through the game port (`joy_*`).
* **Files**: game data lives in archive files (`data\*.dat`, `gfx\*.dat`) read with
  `archive_*`: a directory at the end of the file with names XOR 0x3A, entries optionally
  LZW compressed. Text sections are parsed by `data_load_section` (`END_LIST` terminated).
* **Sound**: HMI SOS 4 for digital effects (`sfx_*`, raw samples from `data\sound.dat`) and
  MIDI music (`.HMP` songs with `melodic.bnk`/`drum.bnk` OPL banks); CD audio tracks via
  MSCDEX device requests.
* **Graphics**: mode 13h, 320x200, back buffer in conventional memory, palette fades via
  `palette_fade`; optional horizontal/vertical mirroring (the MIRRO/UPDOW cheats, bits of `g_race_options`).
