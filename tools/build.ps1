# Build the Pico firmware and the host unit tests.
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1            # firmware + tests
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target firmware
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target tests
#
# Needs: Visual Studio (C++ workload, provides cl, CMake and Ninja), the Pico
# SDK under ~/.pico-sdk/sdk/<ver> (or PICO_SDK_PATH) and the Arm GNU toolchain
# under ~/.pico-sdk/toolchain/<ver> (or PICO_TOOLCHAIN_PATH).
param(
    [ValidateSet('all', 'firmware', 'tests')]
    [string]$Target = 'all'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio with the C++ workload was not found' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

if (-not $env:PICO_TOOLCHAIN_PATH) {
    $tc = Get-ChildItem "$env:USERPROFILE\.pico-sdk\toolchain" -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name | Select-Object -Last 1
    if ($tc) { $env:PICO_TOOLCHAIN_PATH = $tc.FullName }
}

function Invoke-InVsEnv([string]$commands) {
    cmd /c "call `"$vcvars`" >nul 2>&1 && cd /d `"$root`" && $commands"
    if ($LASTEXITCODE -ne 0) { throw "command failed ($LASTEXITCODE): $commands" }
}

if ($Target -in 'all', 'firmware') {
    if (-not $env:PICO_TOOLCHAIN_PATH) { throw 'Arm GNU toolchain not found; set PICO_TOOLCHAIN_PATH' }
    Invoke-InVsEnv 'cmake -S firmware -B build\firmware -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build\firmware'
    Write-Host "firmware: $root\build\firmware\hidway_fw.uf2"
}

if ($Target -in 'all', 'tests') {
    Invoke-InVsEnv 'cmake -S . -B build\host -G Ninja && cmake --build build\host && ctest --test-dir build\host --output-on-failure'
}
