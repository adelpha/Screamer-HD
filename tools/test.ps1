<#
.SYNOPSIS
  Build, then run the CTest suite. Exit code is non-zero if anything fails.
.EXAMPLE
  .\tools\test.ps1                      # everything
  .\tools\test.ps1 -Label render        # only one label: unit, render, smoke, gpu, physics, gamedata
  .\tools\test.ps1 -Filter testpattern  # ctest -R regex
#>
param(
    [ValidateSet('debug', 'release', 'dist')] [string] $Preset = 'debug',
    [string] $Label,
    [string] $Filter,
    [switch] $NoBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $NoBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -Preset $Preset }
. (Join-Path $PSScriptRoot 'env.ps1')

Push-Location $root
try {
    $ctestArgs = @('--preset', $Preset)
    if ($Label) { $ctestArgs += @('-L', $Label) }
    if ($Filter) { $ctestArgs += @('-R', $Filter) }
    ctest @ctestArgs
    exit $LASTEXITCODE
} finally { Pop-Location }
