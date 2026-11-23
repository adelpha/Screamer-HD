# Dot-source this to get an MSVC x64 + CMake + Ninja environment in the current PowerShell.
#   . .\tools\env.ps1
# Idempotent: does nothing if cl.exe is already on PATH.

$ErrorActionPreference = 'Stop'

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio 2022 Build Tools with the C++ workload.' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio install with the MSVC x64 toolset was found.' }

    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    # Capture the environment vcvars64 produces and import it into this session.
    $lines = & cmd.exe /s /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $lines) {
        $i = $line.IndexOf('=')
        if ($i -gt 0) { Set-Item -Path ("env:" + $line.Substring(0, $i)) -Value $line.Substring($i + 1) }
    }

    # vcvars normally adds the bundled CMake/Ninja; make sure.
    $cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    foreach ($d in @("$cmakeDir\CMake\bin", "$cmakeDir\Ninja")) {
        if ((Test-Path $d) -and ($env:PATH -notlike "*$d*")) { $env:PATH = "$d;$env:PATH" }
    }
}

foreach ($tool in 'cl.exe', 'cmake.exe', 'ninja.exe') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found after environment setup." }
}
