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

Every run then applies `reference/symbols/<Exe>.sym` with `ApplySymbols.java` (names,
calling conventions, prototypes, comments, data types) and, with `-Dump`, writes these to
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

See `reference/symbols/STARTL.sym` for every name; about half of the 1,032 functions are
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
  `g_game_mode` to `race_update_*`, renders with `render_set_camera` + `render_scene_alt`,
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
  `palette_fade`; optional horizontal/vertical mirroring (`g_display_flags`).
