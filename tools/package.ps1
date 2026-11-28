<#
.SYNOPSIS
  Build the release package: artifacts\release\ScreamerHD-<version>-win64.zip.
.DESCRIPTION
  Builds the "dist" preset (C/C++ runtime linked in, no console window, no build-machine game
  path), runs the whole test suite on exactly those binaries (headless; the original game's
  files from SCREAMER\ when they are there), checks that no binary needs the Visual C++
  redistributable, and zips the game with README.md and the third-party licences. Nothing of
  the original game goes in: the zip's file list is checked against what we put there.
.EXAMPLE
  .\tools\package.ps1
  .\tools\package.ps1 -SkipTests
#>
param(
    [switch] $SkipTests,
    [switch] $Clean
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'env.ps1')

Push-Location $root
try {
    $m = Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(ScreamerHD VERSION ([0-9.]+)'
    if (-not $m) { throw 'version not found in CMakeLists.txt' }
    $version = $m.Matches[0].Groups[1].Value
    $short = $version -replace '\.0$', ''   # 1.0.0 -> 1.0

    & (Join-Path $PSScriptRoot 'build.ps1') -Preset dist -Clean:$Clean
    $bin = Join-Path $root 'build\dist\bin'
    $deps = Join-Path $root 'build\dist\_deps'

    if (-not $SkipTests) {
        $data = Join-Path $root 'SCREAMER'
        if (Test-Path $data) { $env:SCREAMER_DATA = $data }  # (the package has no game path of its own)
        ctest --preset dist
        if ($LASTEXITCODE) { throw "tests failed ($LASTEXITCODE)" }
    }

    # No binary may need the Visual C++ runtime DLLs.
    $files = @('screamer.exe', 'texextract.exe', 'SDL3.dll', 'opl3.dll')
    foreach ($f in $files) {
        $p = Join-Path $bin $f
        if (-not (Test-Path $p)) { throw "missing $p" }
        $dlls = (& dumpbin /nologo /dependents $p) | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
        $bad = $dlls | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbase|api-ms-win-crt)' }
        if ($bad) { throw "$f needs the Visual C++ runtime: $($bad -join ', ')" }
        Write-Host ("{0}: {1}" -f $f, ($dlls -join ', '))
    }
    $sub = (& dumpbin /nologo /headers (Join-Path $bin 'screamer.exe')) | Select-String 'subsystem \(Windows GUI\)'
    if (-not $sub) { throw 'screamer.exe is not a Windows-subsystem program' }

    # Stage
    $name = "ScreamerHD-$short-win64"
    $out = Join-Path $root 'artifacts\release'
    $stage = Join-Path $out $name
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage, "$stage\LICENSES", "$stage\SCREAMER", "$stage\third_party\Nuked-OPL3-fast" | Out-Null
    foreach ($f in $files) { Copy-Item (Join-Path $bin $f) $stage }
    Copy-Item (Join-Path $root 'README.md') $stage
    Copy-Item (Join-Path $deps 'sdl3-src\LICENSE.txt') "$stage\LICENSES\SDL3.txt"
    $opl = Join-Path $deps 'nuked_opl3-src'
    $oplLicence = Get-ChildItem $opl -File | Where-Object { $_.Name -match '^(LICENSE|COPYING)' } | Select-Object -First 1
    if (-not $oplLicence) { throw "no licence file in $opl" }
    Copy-Item $oplLicence.FullName "$stage\LICENSES\Nuked-OPL3-fast.txt"
    # the decoders of the MUSIC folder's ripped tracks (public domain or MIT-0 / MIT, built in)
    Copy-Item (Join-Path $deps 'dr_libs-src\LICENSE') "$stage\LICENSES\dr_libs.txt"
    Copy-Item (Join-Path $deps 'stb-src\LICENSE') "$stage\LICENSES\stb_vorbis.txt"
    # LGPL-2.1: the source of the opl3.dll we ship, beside it
    foreach ($f in 'opl3.c', 'opl3.h') { Copy-Item (Join-Path $opl $f) "$stage\third_party\Nuked-OPL3-fast" }
    Copy-Item $oplLicence.FullName "$stage\third_party\Nuked-OPL3-fast\$($oplLicence.Name)"
    $commit = (Select-String -Path (Join-Path $root 'cmake\Dependencies.cmake') -Pattern 'GIT_TAG\s+([0-9a-f]{40})').Matches[0].Groups[1].Value
    @"
opl3.dll is built from Nuked-OPL3-fast (https://github.com/tgies/Nuked-OPL3-fast),
commit $commit, unmodified: these two files, compiled as a DLL with
WINDOWS_EXPORT_ALL_SYMBOLS. It is licensed under the GNU LGPL 2.1 (see the licence file here).
You may replace opl3.dll with your own build of the same source.
"@ | Set-Content -Encoding ascii "$stage\third_party\Nuked-OPL3-fast\README.txt"
    @"
Put the files of your own copy of Screamer (1995, DOS / GOG / Steam) in this folder:
the game's folder with GFX, DATA and CONFIG in it, and for CD music its CD image
(SCREAM.cue + SCREAM.bin, or SCREAM.DAT + SCREAM.gog as GOG and Steam ship it)
or the CD's music tracks ripped to MP3/FLAC/OGG/WAV in a MUSIC folder here
(named by CD track number, e.g. MUSIC\02.flac ... MUSIC\08.flac).

So that this folder holds GFX\, DATA\, ... directly (not SCREAMER\Screamer\GFX).
See README.md.
"@ | Set-Content -Encoding ascii "$stage\SCREAMER\PUT_YOUR_SCREAMER_FILES_HERE.txt"
    New-Item -ItemType Directory -Force "$stage\textures" | Out-Null
    @"
Replacement textures for the hardware renderer (OPTIONS > VIDEO > RENDER HARDWARE,
ADVANCED > TEXTURES CUSTOM). Run texextract.exe once: it writes every texture of the
game here as a PNG (cars, objects, tracks with their road, scenery and sky). Edit or
replace any of them, at any whole multiple of the size, keeping the end of the file
name (_<12 hex digits>.png, or _f<N> for an animation frame); delete the rest. See
README.md.
"@ | Set-Content -Encoding ascii "$stage\textures\README.txt"

    # Only what we put there
    $expected = @('screamer.exe', 'texextract.exe', 'textures\README.txt', 'SDL3.dll', 'opl3.dll', 'README.md', 'LICENSES\SDL3.txt', 'LICENSES\Nuked-OPL3-fast.txt',
                  'LICENSES\dr_libs.txt', 'LICENSES\stb_vorbis.txt',
                  'SCREAMER\PUT_YOUR_SCREAMER_FILES_HERE.txt', 'third_party\Nuked-OPL3-fast\opl3.c',
                  'third_party\Nuked-OPL3-fast\opl3.h', 'third_party\Nuked-OPL3-fast\README.txt',
                  "third_party\Nuked-OPL3-fast\$($oplLicence.Name)")
    $actual = Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1) }
    $extra = $actual | Where-Object { $expected -notcontains $_ }
    if ($extra) { throw "unexpected files in the package: $($extra -join ', ')" }

    $zip = Join-Path $out "$name.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path $stage -DestinationPath $zip
    # Debug symbols, separately (for crash reports)
    $pdbZip = Join-Path $out "$name-pdb.zip"
    if (Test-Path $pdbZip) { Remove-Item -Force $pdbZip }
    Compress-Archive -Path (Join-Path $bin 'screamer.pdb'), (Join-Path $bin 'SDL3.pdb'), (Join-Path $bin 'opl3.pdb') -DestinationPath $pdbZip

    foreach ($z in $zip, $pdbZip) {
        $h = (Get-FileHash -Algorithm SHA256 $z).Hash.ToLower()
        Write-Host ("{0}  {1}  ({2:N1} MB)" -f $h, (Split-Path -Leaf $z), ((Get-Item $z).Length / 1MB)) -ForegroundColor Green
    }
} finally { Pop-Location }
