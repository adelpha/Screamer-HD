# Notes for Claude

Decompilation / modernisation of the 1995 DOS game Screamer to native Windows with SDL3.

## Never commit original game data
`SCREAMER/` (the user's original install, including the CD image), `ghidra_*/`,
`tools/bin/` and `artifacts/` are git-ignored. Do not add game files, extracted assets,
disassembly dumps, or Ghidra projects to git. Hashes, notes and our own code are fine.

## Build and verify without the user
- Build: `.\tools\build.ps1 [-Preset release]` (sets up MSVC from VS 2022 Build Tools itself).
- Test: `.\tools\test.ps1` (all), `-Label unit|render|smoke|gamedata`, `-Filter <regex>`.
- Run headless and look at the result: `.\tools\run.ps1 --headless --frames N --screenshot artifacts\x.png`
  (or `--dump-fb` for the native 320x200 frame), then Read the PNG.
- Car physics against the original: `.\tools\dosbox\record-race.ps1 -Out artifacts\traces\x.trace -Frames 450 -Script '300:accel,100:accel+left' [-Skip N]`
  records the player car every frame of a real race (MAP1); `build\debug\bin\tracecmp.exe artifacts\traces\x.trace SCREAMER\GFX\MAP1H.DAT MAP1`
  replays each frame through src/core/car and lists differences. Traces in artifacts/traces become `physics.*` tests.
- Golden images: `.\tools\update-golden.ps1` only for intentional rendering changes; view the new images first.
- Original game reference: `.\tools\dosbox\run-original.ps1 -Name x -Autotype '-w 4 esc' -Shots 9 -Duration 10`
  then Read `artifacts\dosbox\x\shot_*.png`. Add `-Webserver` and use `tools\dosbox\read-memory.ps1`
  for memory/CPU state.
- Ghidra headless: `.\tools\ghidra-headless.ps1 <projdir> <name> ...` (keep projects under artifacts\). Python: `py -3.14`.
- Reverse engineering: `.\tools\ghidra\apply-symbols.ps1 [-Fresh] -Dump` imports STARTL.EXE (via tools/le/le2elf.py),
  applies `reference/symbols/STARTL.sym` (the source of truth for names) and dumps decomp/disasm to
  artifacts\ghidra\STARTL\dump. `.\tools\ghidra\runtime-probe.ps1` reads globals from the running original.
  Details: docs/REVERSING.md.
- Compare images: `build\debug\bin\imgcmp.exe expected.png actual.png --diff d.png`.
- Renderer changes that must not alter the picture: `.\tools\render-compare.ps1 -Out artifacts\perf\new -Against artifacts\perf\base`
  (23 headless scenes against the frames of the build before, made with `-Exe <old exe> -Out artifacts\perf\base`);
  speed: `.\tools\render-bench.ps1 [-Exe old.exe, new.exe]`. `--hidden` runs like `--headless` on the real
  renderer (graphics card) with a window that is never shown.

Full details: docs/HARNESS.md.

## Conventions
- C++20, MSVC /W4 clean. Engine code under `src/core/` stays free of window/frontend code.
- Headless runs must be deterministic (each frame = 2 ticks of the original's 100 Hz clock,
  see src/core/sim_clock.h; output depends only on frame count).
- DOSBox runs must be headless (`run-original.ps1 -Headless`); the user doesn't want windows popping up.
  Window-free screen captures of the original: `.\tools\dosbox\capture-vga.ps1 -Name x -Delay 9 -Autotype '-w 4 esc'`.
- New unit tests go in `tests/unit/test_*.cpp` and must be listed in `tests/CMakeLists.txt`;
  tests needing original data call `SKIP(...)` when it is absent so CI passes.
