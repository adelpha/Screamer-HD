<#
.SYNOPSIS
  Starts a race in the original game (headless DOSBox), then records the player car frame by
  frame with scripted controls (tools/dosbox/step_race.py).
.DESCRIPTION
  The default -Autotype goes from the intro to a new game, accepts the default name and lands
  on the first race (MAP1). The race is stopped one frame at a time, so the recording does
  not depend on DOSBox's speed; each record holds the frame time the game used.
.EXAMPLE
  .\tools\dosbox\record-race.ps1 -Out artifacts\traces\map1.trace -Frames 400 -Script '300:accel,60:accel+left,40:brake'
#>
param(
    [string]$Out = 'artifacts\traces\race.trace',
    [int]$Frames = 300,
    [string]$Script = '300:accel',
    [int]$Skip = 0,                 # frames driven (following -Script) before recording starts
    [double]$Delay = 16,
    [string]$Autotype = '-w 4 esc -w 2 enter -w 1 enter enter enter enter enter enter enter enter enter enter -w 1 enter'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
New-Item -ItemType Directory -Force (Split-Path -Parent $outPath) | Out-Null
$runner = Join-Path $PSScriptRoot 'run-original.ps1'
$duration = $Delay + 30 + ($Frames + $Skip) * 0.5
$job = Start-Job -ArgumentList $runner, $Autotype, $duration {
    param($runner, $autotype, $duration)
    & $runner -Name record-race -Program 'STARTL.EXE' -Autotype $autotype -Webserver -Headless -Duration $duration
}
$api = 'http://127.0.0.1:8086/api/v1'
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Delay) { Start-Sleep -Milliseconds 250 }
for ($i = 0; $i -lt 40; $i++) {
    try { Invoke-RestMethod "$api/cpu/state" | Out-Null; break } catch { Start-Sleep -Milliseconds 250 }
}
try {
    & py -3.14 (Join-Path $PSScriptRoot 'step_race.py') $outPath $Frames $Script $Skip
} finally {
    Stop-Job $job -ErrorAction SilentlyContinue
    Remove-Job $job -Force
    Get-Process dosbox -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force
}
