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
    [string]$Program = 'STARTL.EXE'
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
# VGA memory first (the moment that matters), then 15 MB of extended memory to find the palette.
Invoke-WebRequest "$api/memory/655360/64000" -UseBasicParsing -OutFile (Join-Path $out 'vga.raw')
for ($mb = 1; $mb -lt 16; $mb++) {
    Invoke-WebRequest ("$api/memory/{0}/1048576" -f ($mb * 0x100000)) -UseBasicParsing -OutFile (Join-Path $out ('mem_{0:d2}.bin' -f $mb))
}
Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force
Get-Process dosbox -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force

& py -3.14 (Join-Path $PSScriptRoot 'vga_to_bmp.py') (Join-Path $root 'artifacts\ghidra\STARTL\STARTL.elf') $out
Get-ChildItem $out -Filter 'mem_*.bin' | Remove-Item -Force
