<#
.SYNOPSIS
  Captures the original game's 320x200 screen without a window: runs DOSBox headless,
  waits, then reads VGA memory (A000:0000) and the game's palette from its own memory
  (g_palette, found through the symbol map) and writes a BMP.
.DESCRIPTION
  The palette comes from the game's g_palette, i.e. the palette at full brightness. During a
  fade the real DAC values differ; capture fully faded-in screens for comparisons.
  Requires artifacts\ghidra\STARTL\STARTL.elf (run tools\ghidra\apply-symbols.ps1 -Fresh once).
.EXAMPLE
  .\tools\dosbox\capture-vga.ps1 -Name title -Delay 9 -Autotype '-w 4 esc'
  -> artifacts\dosbox\title\vga.bmp
#>
param(
    [string]$Name = 'vga',
    [double]$Delay = 9,
    [string]$Autotype = '',
    [string]$Program = 'STARTL.EXE',
    [switch]$KeepMemory,  # keep the 15 MB memory dump (mem_*.bin) for runtime_probe.py
    [switch]$Freeze       # STARTL races (and the intro demo) only: stop the game right after it
                          # renders the 3D scene and capture its back buffer (no HUD) instead of
                          # VGA, so screen and memory (camera, object instances) match exactly
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $root "artifacts\dosbox\$Name"
New-Item -ItemType Directory -Force $out | Out-Null
$runner = Join-Path $PSScriptRoot 'run-original.ps1'
$job = Start-Job -ArgumentList $runner, $Program, $Autotype, ($Delay + 15), $Name {
    param($runner, $program, $autotype, $duration, $name)
    & $runner -Name "$name-run" -Program $program -Autotype $autotype -Webserver -Headless -Duration $duration
}
$api = 'http://127.0.0.1:8086/api/v1'
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Delay) { Start-Sleep -Milliseconds 250 }
for ($i = 0; $i -lt 40; $i++) {
    try { Invoke-RestMethod "$api/cpu/state" | Out-Null; break } catch { Start-Sleep -Milliseconds 250 }
}
if ($Freeze) {
    # race_frame calls render_race_scene at 0x1FFE7 and attract_mode_update_cars (the intro
    # demo) at 0x27930; the car physics runs after it and the frame reaches VGA even later.
    # JMP $ (EB FE) at the instruction after each call hangs the game with the scene in the back
    # buffer and the state it was drawn from in memory. Code runs at +0x1C2000 and data at
    # +0x1AE000 under the harness config (pointers are linear addresses).
    foreach ($at in 0x1FFEC, 0x27935) {
        Invoke-WebRequest "$api/memory/$($at + 0x1C2000)" -Method Put -ContentType 'application/octet-stream' `
            -Body ([byte[]](0xEB, 0xFE)) -UseBasicParsing | Out-Null
    }
    Start-Sleep -Milliseconds 300
    $tmp = Join-Path $out 'back_ptr.bin'
    Invoke-WebRequest "$api/memory/$(0x121524 + 0x1AE000)/4" -UseBasicParsing -OutFile $tmp  # g_back_buffer
    $back = [BitConverter]::ToUInt32([System.IO.File]::ReadAllBytes($tmp), 0)
    Remove-Item $tmp
    Invoke-WebRequest "$api/memory/$back/64000" -UseBasicParsing -OutFile (Join-Path $out 'vga.raw')
} else {
    # VGA memory first (the moment that matters), then 15 MB of extended memory to find the palette.
    Invoke-WebRequest "$api/memory/655360/64000" -UseBasicParsing -OutFile (Join-Path $out 'vga.raw')
}
# The game's data object lands at 0x2AE000 under the harness config, so read that MB right
# after the screen to keep camera and screen in step, then the first MB (the 3D object
# instances live in conventional memory); the rest only serves the palette search.
$order = @(2, 0) + (1..15 | Where-Object { $_ -ne 2 })
foreach ($mb in $order) {
    Invoke-WebRequest ("$api/memory/{0}/1048576" -f ($mb * 0x100000)) -UseBasicParsing -OutFile (Join-Path $out ('mem_{0:d2}.bin' -f $mb))
}
if ($Freeze) {
    $after = Join-Path $out 'vga_after.raw'
    Invoke-WebRequest "$api/memory/$back/64000" -UseBasicParsing -OutFile $after
    $same = [System.Linq.Enumerable]::SequenceEqual([System.IO.File]::ReadAllBytes((Join-Path $out 'vga.raw')), [System.IO.File]::ReadAllBytes($after))
    Remove-Item $after
    if (-not $same) { Write-Warning 'the back buffer changed during the capture: the game did not freeze (not in a race?)' }
}
Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force
Get-Process dosbox -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force

& py -3.14 (Join-Path $PSScriptRoot 'vga_to_bmp.py') (Join-Path $root 'artifacts\ghidra\STARTL\STARTL.elf') $out
if (-not $KeepMemory) { Get-ChildItem $out -Filter 'mem_*.bin' | Remove-Item -Force }
