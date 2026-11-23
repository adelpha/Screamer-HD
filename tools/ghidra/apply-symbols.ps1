<#
.SYNOPSIS
  Rebuilds or updates the Ghidra project for a Screamer executable and applies the symbol map.
.DESCRIPTION
  -Fresh   converts the LE executable to ELF (tools/le/le2elf.py), imports it with full
           auto-analysis, installs the __watcall convention and marks LE fixup pointers.
  Always:  applies reference/symbols/<Exe>.sym and, with -Dump, writes analysis dumps
           (functions.tsv, calls.tsv, disasm.txt, decomp.c) to artifacts/ghidra/<Exe>/dump.
  Everything generated lives under artifacts/ (git-ignored): it is derived from the game.
.EXAMPLE
  .\tools\ghidra\apply-symbols.ps1 -Fresh -Dump
  .\tools\ghidra\apply-symbols.ps1 -Exe STARTH
#>
param(
    [string]$Exe = 'STARTL',
    [switch]$Fresh,
    [switch]$Dump,
    [string]$Decompile = 'all'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$data = if ($env:SCREAMER_DATA) { $env:SCREAMER_DATA } else { Join-Path $root 'SCREAMER' }
$work = Join-Path $root "artifacts\ghidra\$Exe"
$proj = Join-Path $work 'proj'
$elf = Join-Path $work "$Exe.elf"
$scripts = Join-Path $PSScriptRoot 'scripts'
$sym = Join-Path $root "reference\symbols\$Exe.sym"
$headless = Join-Path $root 'tools\ghidra-headless.ps1'
New-Item -ItemType Directory -Force $proj | Out-Null

if ($Fresh -or -not (Test-Path (Join-Path $proj 'screamer.gpr'))) {
    & py -3.14 (Join-Path $root 'tools\le\le2elf.py') (Join-Path $data "$Exe.EXE") $elf
    if ($LASTEXITCODE) { throw 'le2elf failed' }
    Remove-Item -Recurse -Force (Join-Path $proj 'screamer.*') -ErrorAction SilentlyContinue
    & $headless $proj screamer -import $elf -processor x86:LE:32:default -cspec gcc `
        -scriptPath $scripts -postScript ScreamerInit.java ($elf -replace '\.elf$', '.fixups.json') `
        -log (Join-Path $work 'import.log')
    if ($LASTEXITCODE) { throw 'Ghidra import failed' }
}

$post = @()
Remove-Item (Join-Path $work 'apply.log') -ErrorAction SilentlyContinue
if (Test-Path $sym) { $post += @('-postScript', 'ApplySymbols.java', $sym) }
if ($Dump) { $post += @('-postScript', 'DumpAnalysis.java', (Join-Path $work 'dump'), $Decompile) }
& $headless $proj screamer -process "$Exe.elf" -noanalysis -scriptPath $scripts @post -log (Join-Path $work 'apply.log')
if ($LASTEXITCODE) { throw 'Ghidra apply failed' }
Select-String -Path (Join-Path $work 'apply.log') -Pattern 'ApplySymbols:|ApplySymbols.java>.*:\d+:' | ForEach-Object { $_.Line }
