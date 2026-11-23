<#
.SYNOPSIS
  Runs Ghidra's analyzeHeadless with JDK 21+ (the machine's default JAVA_HOME is Java 8,
  which Ghidra 12 cannot use). All arguments are passed through.
.EXAMPLE
  .\tools\ghidra-headless.ps1 E:\screamer_recomp\artifacts\ghidra screamer -import SCREAMER\STARTL.EXE
  Ghidra projects contain the disassembled original, so keep them under artifacts\ (git-ignored).
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ghidra = Get-ChildItem $root -Directory -Filter 'ghidra_*' | Sort-Object Name | Select-Object -Last 1
if (-not $ghidra) { throw "No ghidra_* folder found in $root" }
$jdk = Get-ChildItem 'C:\Program Files\Eclipse Adoptium', 'C:\Program Files\Java' -Directory -Filter 'jdk-*' -ErrorAction SilentlyContinue |
    Where-Object { [int](($_.Name -replace '^jdk-?', '') -split '\.')[0] -ge 21 } | Sort-Object Name | Select-Object -Last 1
if (-not $jdk) { throw 'JDK 21+ not found under Program Files\Eclipse Adoptium or Program Files\Java' }
$env:JAVA_HOME = $jdk.FullName
$env:PATH = "$($jdk.FullName)\bin;$env:PATH"
& (Join-Path $ghidra.FullName 'support\analyzeHeadless.bat') @args
exit $LASTEXITCODE
