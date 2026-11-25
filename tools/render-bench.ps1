<#
.SYNOPSIS
  Times the drawing of a fixed headless race (--perf) and prints the best and the median of
  several runs for each part of the loop. Several -Exe values are run turn by turn, so a
  busy moment of the PC hits all of them alike.
.EXAMPLE
  .\tools\render-bench.ps1                                                    # the release build
  .\tools\render-bench.ps1 -Exe artifacts\perf\base_bin\screamer.exe, build\release\bin\screamer.exe
  .\tools\render-bench.ps1 -ExtraArgs '--threads', '1' -Size 1600x1000
  .\tools\render-bench.ps1 -Hidden                                            # on the graphics card
.NOTES
  Headless, the "texture upload" and "present" lines are SDL's software renderer, not the
  graphics card of a real run; -Hidden gives those.
#>
param(
    [string[]] $Exe,
    [string] $Size = '1600x1000',
    [string] $Track = 'MAP1',
    [int] $Frames = 400,
    [int] $Runs = 5,
    [string[]] $ExtraArgs = @(),
    # the real video driver and renderer (graphics card) on a window that is never shown,
    # instead of the software renderer: real upload and present times (builds with --hidden)
    [switch] $Hidden,
    # one string of extra arguments per -Exe (split at spaces), to compare options of one build
    [string[]] $PerExeArgs = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = @(Join-Path $root 'build\release\bin\screamer.exe') }
$sections = '3D geometry', '3D fill', 'HUD/overlays', 'texture upload', 'present+vsync', 'render3d', 'frame'
$results = @{}
Push-Location $root
try {
    foreach ($run in 1..$Runs) {
        for ($i = 0; $i -lt $Exe.Count; $i++) {
            $a = @($(if ($Hidden) { '--hidden' } else { '--headless' }), '--mute', '--scene', 'drive', '--track', $Track, '--hold', 'accel', '--frames', "$Frames",
                   '--render-size', $Size, '--perf') + $ExtraArgs
            if ($i -lt $PerExeArgs.Count -and $PerExeArgs[$i]) { $a += $PerExeArgs[$i] -split ' ' }
            $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
            $log = & $Exe[$i] @a 2>&1 | ForEach-Object { "$_" }
            $ErrorActionPreference = $prev
            foreach ($s in $sections) {
                # the last report covers the frames since the one before; the whole-run line for render3d
                $pattern = if ($s -eq 'render3d') { 'render3d: ([\d.]+) ms average' } elseif ($s -eq 'frame') { 'frame ([\d.]+) ms average' } else { [regex]::Escape($s) + '\s+([\d.]+) ms/frame' }
                $all = @($log | Select-String $pattern)
                if (-not $all.Count) { continue }
                $vals = $all | ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
                $v = ($vals | Measure-Object -Average).Average
                $key = "$i|$s"
                if (-not $results.ContainsKey($key)) { $results[$key] = @() }
                $results[$key] += $v
            }
        }
    }
} finally { Pop-Location }
for ($i = 0; $i -lt $Exe.Count; $i++) {
    $extra = if ($i -lt $PerExeArgs.Count) { " $($PerExeArgs[$i])" } else { '' }
    Write-Host "$($Exe[$i])$extra  ($Size, $Track, $Frames frames, $Runs runs; ms per frame: best / median)"
    foreach ($s in $sections) {
        $v = @($results["$i|$s"] | Sort-Object)
        if (-not $v.Count) { continue }
        $name = if ($s -eq 'render3d') { '3D total (geometry + fill)' } elseif ($s -eq 'frame') { 'whole frame' } elseif ($s -eq 'present+vsync') { 'present (no vsync)' } else { $s }
        Write-Host ('  {0,-28} {1,7:F3} / {2,7:F3}' -f $name, $v[0], $v[[int][math]::Floor(($v.Count - 1) / 2)])
    }
}
