# package.ps1 - Build the Windows release package (spec §17, Task 21).
#
# Steps: create out dir -> copy hlm_app.exe + plc_simulator.exe -> run
# windeployqt on hlm_app.exe -> copy OpenSSL/OpenCV DLLs from the vcpkg
# installed tree -> copy LICENSES/ -> Compress-Archive to
# plc-hmi-<Version>-win64.zip. Non-zero exit on any failure.
#
# Usage:
#   pwsh -NoProfile -File scripts/package.ps1 `
#     -BuildDir build -OutDir dist -Config Release `
#     -VcpkgInstalledDir "$env:VCPKG_ROOT/installed/x64-windows" `
#     -Version 0.1.0
#
# Parameters:
#   -BuildDir          CMake build directory. Supports both single-config
#                      layouts (build/hlm_app.exe) and multi-config layouts
#                      (build/<Config>/hlm_app.exe).
#   -OutDir            Output directory for the package and zip.
#   -Config            Build configuration, default Release.
#   -VcpkgInstalledDir vcpkg installed tree (installed/x64-windows). Used for
#                      OpenSSL and OpenCV DLLs.
#   -Version           Package version, default 0.1.0 (matches CMake project).
#   -QtBinDir          Optional Qt bin directory; if omitted, windeployqt is
#                      located via Get-Command or probed under the vcpkg
#                      installed tree (tools/qtbase/bin).

param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [Parameter(Mandatory = $true)]
    [string]$OutDir,
    [string]$Config = "Release",
    [string]$VcpkgInstalledDir = "",
    [string]$Version = "0.1.0",
    [string]$QtBinDir = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Fail([string]$msg) {
    Write-Error $msg
    exit 1
}

# --- Locate binaries ---------------------------------------------------------
# Visual Studio and Ninja Multi-Config place executables under <Config>/,
# while the single-config Ninja generator used by CI places them directly in
# the build directory. Prefer the multi-config layout and fall back to the
# single-config layout.
$exeDir = Join-Path $BuildDir $Config
if (-not (Test-Path (Join-Path $exeDir "hlm_app.exe"))) {
    $exeDir = $BuildDir
}

$hlmApp = Join-Path $exeDir "hlm_app.exe"
$plcSim = Join-Path $exeDir "plc_simulator.exe"
if (-not (Test-Path $hlmApp)) {
    Fail "hlm_app.exe not found in $BuildDir or $(Join-Path $BuildDir $Config)"
}
if (-not (Test-Path $plcSim)) {
    Fail "plc_simulator.exe not found in $BuildDir or $(Join-Path $BuildDir $Config)"
}

# --- Locate windeployqt ------------------------------------------------------
$windeployqt = $null
if ($QtBinDir -ne "") {
    $candidate = Join-Path $QtBinDir "windeployqt.exe"
    if (Test-Path $candidate) { $windeployqt = $candidate }
}
if (-not $windeployqt) {
    $cmd = Get-Command windeployqt -ErrorAction SilentlyContinue
    if ($cmd) { $windeployqt = $cmd.Source }
}
if (-not $windeployqt -and $VcpkgInstalledDir -ne "") {
    # vcpkg's qtbase installs windeployqt under tools/qtbase/bin (not on PATH).
    $candidate = Join-Path $VcpkgInstalledDir "tools\qtbase\bin\windeployqt.exe"
    if (Test-Path $candidate) { $windeployqt = $candidate }
}
if (-not $windeployqt) { Fail "windeployqt not found (pass -QtBinDir or add Qt bin to PATH)" }

# --- Create output dir and copy executables ----------------------------------
$pkgDir = Join-Path $OutDir "plc-hmi-$Version-win64"
if (Test-Path $pkgDir) { Remove-Item -Recurse -Force $pkgDir }
New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null

Copy-Item $hlmApp $pkgDir
Copy-Item $plcSim $pkgDir
Write-Host "Copied hlm_app.exe and plc_simulator.exe to $pkgDir"

# --- windeployqt on hlm_app.exe ----------------------------------------------
Write-Host "Running windeployqt: $windeployqt"
& $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw `
    (Join-Path $pkgDir "hlm_app.exe")
if ($LASTEXITCODE -ne 0) { Fail "windeployqt failed with exit code $LASTEXITCODE" }

# --- OpenSSL / OpenCV DLLs from vcpkg installed tree --------------------------
if ($VcpkgInstalledDir -ne "") {
    $vcpkgBin = Join-Path $VcpkgInstalledDir "bin"
    if (-not (Test-Path $vcpkgBin)) { Fail "vcpkg bin dir not found: $vcpkgBin" }

    $opensslDlls = @("libcrypto-3-x64.dll", "libssl-3-x64.dll")
    foreach ($dll in $opensslDlls) {
        $src = Join-Path $vcpkgBin $dll
        if (Test-Path $src) {
            Copy-Item $src $pkgDir
            Write-Host "Copied $dll"
        } else {
            Write-Warning "OpenSSL DLL not found in vcpkg tree: $dll"
        }
    }

    $opencvDlls = Get-ChildItem -Path $vcpkgBin -Filter "opencv_core*.dll" -ErrorAction SilentlyContinue
    if ($opencvDlls) {
        foreach ($dll in $opencvDlls) {
            Copy-Item $dll.FullName $pkgDir
            Write-Host "Copied $($dll.Name)"
        }
    } else {
        Write-Warning "No opencv_core*.dll found in vcpkg tree: $vcpkgBin"
    }
} else {
    Write-Warning "-VcpkgInstalledDir not provided; OpenSSL/OpenCV DLLs not copied"
}

# --- LICENSES ----------------------------------------------------------------
$licensesSrc = Join-Path $PSScriptRoot "..\LICENSES"
$licensesSrc = [System.IO.Path]::GetFullPath($licensesSrc)
if (Test-Path $licensesSrc) {
    Copy-Item -Recurse $licensesSrc (Join-Path $pkgDir "LICENSES")
    Write-Host "Copied LICENSES/"
} else {
    Fail "LICENSES directory not found at $licensesSrc"
}

# --- Zip ---------------------------------------------------------------------
$zipPath = Join-Path $OutDir "plc-hmi-$Version-win64.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path $pkgDir -DestinationPath $zipPath
Write-Host "Package created: $zipPath"
exit 0
