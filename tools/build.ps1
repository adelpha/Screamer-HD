<#
.SYNOPSIS
  Configure (if needed) and build. Works from a plain PowerShell: sets up MSVC itself.
.EXAMPLE
  .\tools\build.ps1                 # debug build
  .\tools\build.ps1 -Preset release
  .\tools\build.ps1 -Clean
#>
param(
    [ValidateSet('debug', 'release')] [string] $Preset = 'debug',
    [switch] $Clean,
    [string] $Target
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'env.ps1')

Push-Location $root
try {
    $bin = Join-Path $root "build\$Preset"
    if ($Clean -and (Test-Path $bin)) { Remove-Item -Recurse -Force $bin }
    if (-not (Test-Path (Join-Path $bin 'build.ninja'))) {
        cmake --preset $Preset
        if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
    }
    $buildArgs = @('--build', '--preset', $Preset)
    if ($Target) { $buildArgs += @('--target', $Target) }
    cmake @buildArgs
    if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
    Write-Host "Built: $bin\bin" -ForegroundColor Green
} finally { Pop-Location }
