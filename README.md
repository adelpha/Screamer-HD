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
