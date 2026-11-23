<#
.SYNOPSIS
  Regenerate golden images for the render tests from the current build.
  Only run this when a rendering change is intentional; review the new images
  (git diff shows them as changed binaries) before committing.
#>
param([ValidateSet('debug', 'release')] [string] $Preset = 'debug')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'build.ps1') -Preset $Preset
. (Join-Path $PSScriptRoot 'env.ps1')
Push-Location $root
try {
    $env:SCREAMER_UPDATE_GOLDEN = '1'
    ctest --preset $Preset -L render
    $rc = $LASTEXITCODE
} finally {
    Remove-Item env:SCREAMER_UPDATE_GOLDEN -ErrorAction SilentlyContinue
    Pop-Location
}
exit $rc
