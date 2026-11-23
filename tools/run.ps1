<#
.SYNOPSIS
  Run the built game. Any extra arguments are passed through to screamer.exe.
.EXAMPLE
  .\tools\run.ps1                                         # windowed
  .\tools\run.ps1 --headless --frames 140 --screenshot artifacts\shot.png
  .\tools\run.ps1 -Preset release --fullscreen
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet('debug', 'release')] [string] $Preset = 'debug',
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $GameArgs
)
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\$Preset\bin\screamer.exe"
if (-not (Test-Path $exe)) { throw "Not built: run tools\build.ps1 -Preset $Preset" }
Push-Location $root
try { & $exe @GameArgs; exit $LASTEXITCODE } finally { Pop-Location }
