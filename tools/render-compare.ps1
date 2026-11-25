<#
.SYNOPSIS
  Checks that a change to the renderer leaves every picture exactly as it was: runs a set of
  headless scenes (races on every track, odd sizes, the options that change how pixels are
  filled, a replay, the intro demo, the menu) and saves frames from each; with -Against,
  compares them file by file with the frames of an earlier run.
.EXAMPLE
  .\tools\render-compare.ps1 -Exe artifacts\perf\base_bin\screamer.exe -Out artifacts\perf\base   # before
  .\tools\render-compare.ps1 -Out artifacts\perf\new -Against artifacts\perf\base                 # after
.NOTES
  Needs the original game files. Everything runs headless (no window). The frames are our
  output from the game's data, so they stay under artifacts\ (git-ignored).
#>
param(
    [string] $Exe,
    [Parameter(Mandatory = $true)] [string] $Out,
    [string] $Against,
    [string] $Filter,            # only scenes whose name matches this regex
    [string[]] $ExtraArgs = @()  # added to every run (e.g. --threads 1)
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'build\release\bin\screamer.exe' }
$Exe = (Resolve-Path $Exe).Path
if (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
if ($Against -and -not [IO.Path]::IsPathRooted($Against)) { $Against = Join-Path $root $Against }

$big = '1600x1000'
$scenes = [ordered]@{}
foreach ($n in 1..6) {
    $scenes["race_map$n"] = @('--scene', 'drive', '--track', "MAP$n", '--hold', 'accel', '--frames', '500', '--render-size', $big, '--capture-every', '20')
}
$scenes['reversed_odd_size'] = @('--scene', 'drive', '--track', 'MAP2', '--reversed', '--hold', 'accel,left', '--frames', '300', '--render-size', '1283x801', '--capture-every', '15')
$scenes['original_size']     = @('--scene', 'drive', '--track', 'MAP3', '--hold', 'accel', '--frames', '300', '--capture-every', '10')
$scenes['no_view_limit']     = @('--scene', 'drive', '--track', 'MAP4', '--hold', 'accel', '--frames', '200', '--render-size', $big, '--view-distance', '0', '--capture-every', '20')
$scenes['near_view_lod']     = @('--scene', 'drive', '--track', 'MAP5', '--hold', 'accel', '--frames', '300', '--render-size', '801x500', '--view-distance', '1', '--lod', '--capture-every', '15')
$scenes['flat']              = @('--scene', 'drive', '--track', 'MAP6', '--hold', 'accel', '--frames', '200', '--render-size', '640x400', '--flat', '--capture-every', '20')
$scenes['low_detail']        = @('--scene', 'drive', '--track', 'MAP1', '--detail', 'low', '--hold', 'accel', '--frames', '200', '--render-size', '1001x626', '--capture-every', '20')
$scenes['mirror_updown']     = @('--scene', 'drive', '--track', 'MAP1', '--hold', 'accel,right', '--frames', '200', '--render-size', '802x501', '--race-options', '0x30', '--capture-every', '20')
foreach ($v in 0, 1, 3) {
    $scenes["view$v"] = @('--scene', 'drive', '--track', 'MAP2', '--view', "$v", '--hold', 'accel', '--frames', '300', '--render-size', '1283x802', '--capture-every', '25')
}
$scenes['between_steps']     = @('--scene', 'drive', '--track', 'MAP1', '--hold', 'accel', '--frames', '150', '--render-size', '803x502', '--tween-test', '--capture-every', '10')
$scenes['time_attack']       = @('--scene', 'drive', '--track', 'MAP3', '--game-mode', '4', '--hold', 'accel', '--frames', '300', '--render-size', '900x563', '--capture-every', '25')
$scenes['cones']             = @('--scene', 'drive', '--track', 'MAP1', '--game-mode', '0x20', '--hold', 'accel', '--frames', '400', '--render-size', '900x563', '--capture-every', '25')
$scenes['attract']           = @('--scene', 'attract', '--frames', '700', '--render-size', '1283x801', '--capture-every', '35')
$scenes['menu']              = @('--scene', 'menu', '--frames', '260', '--menu-keys', '40:enter,80:enter,120:right,160:enter,200:enter', '--capture-every', '20')
$scenes['still_cars']        = @('--scene', 'track', '--track', 'MAP1', '--car', 'AUTO00', '--car', 'AUTO03,0,0,0,300,8000', '--frames', '3', '--render-size', $big, '--capture-every', '1')
$rpl = Join-Path $root 'SCREAMER\REPLAYS\REPLAY01.RPL'
if (Test-Path $rpl) {
    $scenes['replay'] = @('--replay', $rpl, '--track', 'MAP1', '--frames', '5600', '--render-size', '1283x801', '--capture-every', '100')
}

New-Item -ItemType Directory -Force $Out | Out-Null
$failed = 0
Push-Location $root
try {
    foreach ($name in $scenes.Keys) {
        if ($Filter -and $name -notmatch $Filter) { continue }
        $dir = Join-Path $Out $name
        if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
        New-Item -ItemType Directory -Force $dir | Out-Null
        $a = @('--headless', '--mute') + $scenes[$name] + @('--capture-dir', $dir, '--dump-fb', (Join-Path $dir 'last.png')) + $ExtraArgs
        $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        $log = & $Exe @a 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        $ErrorActionPreference = $prev
        # the state of the game at the end (must not depend on how the picture is drawn)
        $log | Where-Object { $_ -match '^(car:|frames=|replay:)' } | Set-Content (Join-Path $dir 'state.txt')
        $count = (Get-ChildItem $dir -Filter *.png).Count
        if ($code -ne 0) { Write-Host "FAIL  $name (exit code $code)"; $failed++; continue }
        if (-not $Against) { Write-Host "saved $name ($count frames)"; continue }
        $ref = Join-Path $Against $name
        if (-not (Test-Path $ref)) { Write-Host "FAIL  $name (not in $Against)"; $failed++; continue }
        $diff = @()
        $names = @(Get-ChildItem $ref -File | ForEach-Object Name) + @(Get-ChildItem $dir -File | ForEach-Object Name) | Sort-Object -Unique
        foreach ($f in $names) {
            $x = Join-Path $ref $f; $y = Join-Path $dir $f
            if (-not (Test-Path $x) -or -not (Test-Path $y) -or (Get-FileHash $x).Hash -ne (Get-FileHash $y).Hash) { $diff += $f }
        }
        if ($diff.Count) { Write-Host "FAIL  $name differs: $($diff -join ', ')"; $failed++ }
        else { Write-Host "same  $name ($count frames)" }
    }
} finally { Pop-Location }
if ($failed) { Write-Host "$failed scene(s) failed"; exit 1 }
if ($Against) { Write-Host 'all scenes identical' }
exit 0
