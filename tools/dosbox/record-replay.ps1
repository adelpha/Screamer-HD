<#
.SYNOPSIS
  Plays one of the original's replays (the records page's "R") in the original game,
  headless, and records positions every frame (step_race.py with COMPACT=1).
.DESCRIPTION
  The game runs from a copy of the install (artifacts\dosbox\gamecopy) so the original
  install is never written to. -Row picks the total-time row of the track's records.
.EXAMPLE
  .\tools\dosbox\record-replay.ps1 -Track 0 -Out artifacts\traces\replay01.trace -Frames 5700
#>
param(
    [int]$Track = 0,
    [int]$Row = 0,
    [string]$Out = 'artifacts\traces\replay.trace',
    [int]$Frames = 6000,
    [double]$Delay = 5,
    [int]$Skip = 0,             # frames before recording starts
    [switch]$Full               # whole PlayerCar and CPU cars (RECORD_OPPONENTS; about 65 frames) instead of positions
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
New-Item -ItemType Directory -Force (Split-Path -Parent $outPath) | Out-Null
$copy = Join-Path $root 'artifacts\dosbox\gamecopy'
if (-not (Test-Path (Join-Path $copy 'STARTL.EXE'))) {
    New-Item -ItemType Directory -Force $copy | Out-Null
    Get-ChildItem (Join-Path $root 'SCREAMER') | Where-Object { $_.Extension -notin '.bin', '.cue' } |
        Copy-Item -Destination $copy -Recurse -Force
}
# intro -> main menu -> OPTIONS (5th) -> VIEW RECORDS (6th) -> the track -> the row -> its replay
$keys = @('-w 8 esc -w 3') + @('down') * 4 + @('enter -w 2') + @('down') * 5 + @('enter -w 2') +
        @('down') * $Track + @('enter -w 3') + @('down') * $Row + @('enter')
$autotype = ($keys -join ' ')
$runner = Join-Path $PSScriptRoot 'run-original.ps1'
$duration = 90 + ($Frames + $Skip) * 0.05
$job = Start-Job -ArgumentList $runner, $autotype, $duration, $copy {
    param($runner, $autotype, $duration, $copy)
    & $runner -Name record-replay -Program 'STARTL.EXE' -GameDir $copy -Autotype $autotype -Webserver -Headless -Duration $duration
}
$api = 'http://127.0.0.1:8086/api/v1'
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Delay) { Start-Sleep -Milliseconds 250 }
for ($i = 0; $i -lt 80; $i++) {
    try { Invoke-RestMethod "$api/cpu/state" | Out-Null; break } catch { Start-Sleep -Milliseconds 250 }
}
try {
    if ($Full) { $env:REPLAY = '1'; $env:RECORD_OPPONENTS = '1' } else { $env:COMPACT = '1' }
    $env:STALL_SECONDS = '15'
    & py -3.14 (Join-Path $PSScriptRoot 'step_race.py') $outPath $Frames '1:' $Skip
} finally {
    Remove-Item Env:COMPACT, Env:REPLAY, Env:RECORD_OPPONENTS, Env:STALL_SECONDS -ErrorAction SilentlyContinue
    Stop-Job $job -ErrorAction SilentlyContinue
    Remove-Job $job -Force
    Get-Process dosbox -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force
}
