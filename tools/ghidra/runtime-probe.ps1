<#
.SYNOPSIS
  Runs the original game in DOSBox, dumps emulated memory after a delay and prints the
  values of the data symbols from reference/symbols/<Exe>.sym at their runtime addresses.
.DESCRIPTION
  DOSBox runs headless (SDL offscreen driver): no window appears and nothing takes focus.
  Use it to confirm what a global holds (and so what the code touching it does) in a given
  game state. -Autotype drives the game first (e.g. '-w 4 esc' to reach the main menu).
  The dump stays in artifacts\ghidra\<Exe>\probe (git-ignored).
.EXAMPLE
  .\tools\ghidra\runtime-probe.ps1 -Delay 10                       # during the intro demo race
  .\tools\ghidra\runtime-probe.ps1 -Delay 9 -Autotype '-w 4 esc' -Only g_game_mode,g_track_name
#>
param(
    [string]$Exe = 'STARTL',
    [double]$Delay = 10,
    [string]$Autotype = '',
    [string[]]$Only = @(),
    [int]$Megabytes = 16
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $root "artifacts\ghidra\$Exe\probe"
$elf = Join-Path $root "artifacts\ghidra\$Exe\$Exe.elf"
if (-not (Test-Path $elf)) { throw "Run tools\ghidra\apply-symbols.ps1 -Exe $Exe -Fresh first" }
New-Item -ItemType Directory -Force $out | Out-Null
Get-ChildItem $out -Filter 'mem_*.bin' | Remove-Item -Force

$runner = Join-Path $root 'tools\dosbox\run-original.ps1'
$job = Start-Job -ArgumentList $runner, "$Exe.EXE", $Autotype, ($Delay + 15) {
    param($runner, $program, $autotype, $duration)
    & $runner -Name probe -Program $program -Autotype $autotype -Webserver -Headless -Duration $duration
}
$api = 'http://127.0.0.1:8086/api/v1'
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $Delay) { Start-Sleep -Milliseconds 250 }
for ($i = 0; $i -lt 40; $i++) {
    try { Invoke-RestMethod "$api/cpu/state" | Out-Null; break } catch { Start-Sleep -Milliseconds 250 }
}
for ($mb = 1; $mb -lt $Megabytes; $mb++) {
    Invoke-WebRequest ("$api/memory/{0}/1048576" -f ($mb * 0x100000)) -UseBasicParsing `
        -OutFile (Join-Path $out ('mem_{0:d2}.bin' -f $mb))
}
Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force
Get-Process dosbox -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$root*" } | Stop-Process -Force

$pyArgs = @((Join-Path $root 'tools\ghidra\runtime_probe.py'), $elf, (Join-Path $root "reference\symbols\$Exe.sym"), $out)
if ($Only) { $pyArgs += @('--only', ($Only -join ',')) }
& py -3.14 @pyArgs
