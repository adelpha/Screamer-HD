<#
.SYNOPSIS
  Runs the ORIGINAL game in DOSBox Staging, unattended, and captures reference
  screenshots (and optionally memory) so the recompiled code can be compared
  against real behaviour.

.DESCRIPTION
  - Input is scripted with DOSBox Staging's built-in AUTOTYPE command, which
    feeds the emulated keyboard directly: no window focus needed.
  - Screenshots are taken by sending the DOSBox screenshot hotkey to its window
    at given times. The hotkey is remapped to F11 (no modifier) via a generated
    mapper file so a single posted key press is enough.
  - Captures use 'raw' format: the exact emulated framebuffer at native
    resolution (e.g. 320x200), directly comparable to screamer --dump-fb.
  - -Webserver enables DOSBox's REST API (http://127.0.0.1:8086/api/v1/...)
    for reading/writing emulated memory and CPU state while the game runs.

.EXAMPLE
  # Boot to the menu and grab screenshots at 8s and 12s
  .\tools\dosbox\run-original.ps1 -Name menu -Shots 8,12 -Duration 14

  # Type keys into the game: wait 6s, then Enter, 1s pause, Enter
  .\tools\dosbox\run-original.ps1 -Name race -Autotype '-w 6 -p 1 enter enter' -Shots 15 -Duration 16

  # Leave it running with the REST API on for manual/scripted memory inspection
  .\tools\dosbox\run-original.ps1 -Webserver -Duration 0
#>
param(
    [string]   $Name = 'run',
    [string]   $Program = 'STARTL.EXE',
    [string]   $ProgramArgs = '',
    [string]   $Autotype = '',
    [double[]] $Shots = @(),
    [double]   $Duration = 20,          # seconds before DOSBox is closed; 0 = leave running
    [string]   $Cycles = 'max',         # e.g. 'max', 'fixed 60000' for more repeatable timing
    [string]   $GameDir,
    [string]   $OutDir,
    [switch]   $Webserver,
    [int]      $WebserverPort = 8086,
    [switch]   $Debugger,               # use dosbox_with_debugger.exe (Alt+Pause breaks into the debugger)
    [switch]   $Sound,                  # audio is off by default for unattended runs
    [string[]] $ExtraConfig = @()       # extra 'section.key=value' overrides
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $GameDir) { $GameDir = if ($env:SCREAMER_DATA) { $env:SCREAMER_DATA } else { Join-Path $root 'SCREAMER' } }
if (-not (Test-Path (Join-Path $GameDir 'GFX'))) { throw "Original game not found at $GameDir" }
$dosboxDir = Join-Path $root 'tools\bin\dosbox-staging'
$exe = Join-Path $dosboxDir $(if ($Debugger) { 'dosbox_with_debugger.exe' } else { 'dosbox.exe' })
if (-not (Test-Path $exe)) { throw "DOSBox Staging not found at $dosboxDir" }
if (-not $OutDir) { $OutDir = Join-Path $root "artifacts\dosbox\$Name" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
Get-ChildItem $OutDir -File -ErrorAction SilentlyContinue | Remove-Item -Force

# The CD image is optional (CD audio + any data read from disc).
$cue = Get-ChildItem $GameDir -Filter *.cue -ErrorAction SilentlyContinue | Select-Object -First 1

$mapper = Join-Path $OutDir 'harness.map'
@'
hand_scrshot "key 68"
'@ | Set-Content -Encoding ascii $mapper   # SDL scancode 68 = F11

$autoexec = @("@echo off", "mount c `"$GameDir`"")
if ($cue) { $autoexec += "imgmount d `"$($cue.FullName)`" -t cdrom" }
$autoexec += 'c:'
if ($Autotype) { $autoexec += "autotype $Autotype" }
$autoexec += "$Program $ProgramArgs".Trim()
$autoexec += 'exit'

$conf = Join-Path $OutDir 'harness.conf'
$lines = @(
    '[sdl]', 'fullscreen = false', 'window_size = 960x600', 'mute_when_inactive = off', 'pause_when_inactive = off',
    "mapperfile = $mapper",
    '[dosbox]', 'machine = svga_s3', 'memsize = 16', 'startup_verbosity = quiet',
    '[cpu]', 'core = auto', "cpu_cycles = $Cycles", "cpu_cycles_protected = $Cycles",
    '[capture]', "capture_dir = $OutDir", 'default_image_capture_formats = raw',
    '[mixer]', "nosound = $(if ($Sound) { 'off' } else { 'on' })",
    '[webserver]', "webserver_enabled = $(if ($Webserver) { 'on' } else { 'off' })", "webserver_port = $WebserverPort",
    '[autoexec]') + $autoexec
$lines | Set-Content -Encoding ascii $conf

$sendKey = @'
using System;
using System.Runtime.InteropServices;
public static class DosboxKeys {
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint type);
    public static void Tap(IntPtr hwnd, uint vk) {
        uint sc = MapVirtualKey(vk, 0);
        PostMessage(hwnd, 0x0100, (IntPtr)vk, (IntPtr)(1 | (sc << 16)));                       // WM_KEYDOWN
        System.Threading.Thread.Sleep(60);
        PostMessage(hwnd, 0x0101, (IntPtr)vk, (IntPtr)(1 | (sc << 16) | (3u << 30)));          // WM_KEYUP
    }
}
'@
if (-not ('DosboxKeys' -as [type])) { Add-Type -TypeDefinition $sendKey }

$log = Join-Path $OutDir 'dosbox.log'
$dbArgs = @('--noprimaryconf', '--nolocalconf', '-conf', "`"$conf`"")
foreach ($kv in $ExtraConfig) { $dbArgs += @('--set', "`"$kv`"") }
$p = Start-Process -FilePath $exe -WorkingDirectory $dosboxDir -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err" -ArgumentList $dbArgs
$start = Get-Date
Write-Host "DOSBox pid $($p.Id), output -> $OutDir"

function Get-Hwnd { $p.Refresh(); $p.MainWindowHandle }

$shotIndex = 0
foreach ($t in ($Shots | Sort-Object)) {
    while (((Get-Date) - $start).TotalSeconds -lt $t) {
        if ($p.HasExited) { break }
        Start-Sleep -Milliseconds 50
    }
    if ($p.HasExited) { Write-Warning "DOSBox exited before shot at ${t}s"; break }
    $before = @(Get-ChildItem $OutDir -Filter *.png -ErrorAction SilentlyContinue).Count
    [DosboxKeys]::Tap((Get-Hwnd), 0x7A)  # VK_F11
    $deadline = (Get-Date).AddSeconds(3)
    while (@(Get-ChildItem $OutDir -Filter *.png -ErrorAction SilentlyContinue).Count -le $before -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
    $new = Get-ChildItem $OutDir -Filter *.png | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($new -and @(Get-ChildItem $OutDir -Filter *.png).Count -gt $before) {
        $dest = Join-Path $OutDir ('shot_{0:D2}_{1:N1}s.png' -f $shotIndex, $t)
        Move-Item $new.FullName $dest -Force
        Write-Host "shot $shotIndex at ${t}s -> $dest"
    } else { Write-Warning "no screenshot captured at ${t}s" }
    $shotIndex++
}

if ($Duration -gt 0) {
    while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $Duration) { Start-Sleep -Milliseconds 100 }
    if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
    Write-Host "DOSBox closed after $([int]((Get-Date) - $start).TotalSeconds)s"
} else {
    Write-Host "DOSBox left running (pid $($p.Id))"
}
