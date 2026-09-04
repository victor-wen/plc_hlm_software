# package.ps1 - Build the Windows release package (spec §17, Task 21).
#
# Steps: create out dir -> copy hlm_app.exe + plc_simulator.exe -> run
# windeployqt on hlm_app.exe -> copy the complete vcpkg runtime DLL set ->
# copy the MSVC runtime DLLs -> copy LICENSES/ -> Compress-Archive to
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
#                      all non-system runtime DLLs, including transitive Qt
#                      dependencies such as FreeType and HarfBuzz.
#   -Version           Package version, default 0.1.0 (matches CMake project).
#   -QtBinDir          Optional Qt bin directory; if omitted, windeployqt is
#                      located via Get-Command or probed under the vcpkg
#                      installed tree (tools/Qt6/bin).

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
    # Current vcpkg Qt 6 ports install host tools under tools/Qt6/bin.
    $candidate = Join-Path $VcpkgInstalledDir "tools\Qt6\bin\windeployqt.exe"
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

# --- vcpkg runtime DLLs -------------------------------------------------------
# windeployqt deploys Qt modules and plugins, but a dynamically linked vcpkg Qt
# build also depends on non-Qt DLLs (FreeType, HarfBuzz, libpng, PCRE2, zlib,
# etc.). Some deployed plugins have their own third-party dependencies as well.
# Copy the release runtime bin as a complete set so no indirect dependency is
# omitted. Manifest mode keeps this directory scoped to this project's declared
# dependency graph; debug DLLs live in a separate debug/bin directory.
if ($VcpkgInstalledDir -ne "") {
    $vcpkgBin = Join-Path $VcpkgInstalledDir "bin"
    if (-not (Test-Path $vcpkgBin)) { Fail "vcpkg bin dir not found: $vcpkgBin" }

    $vcpkgRuntimeDlls = Get-ChildItem -Path $vcpkgBin -Filter "*.dll" -File
    if (-not $vcpkgRuntimeDlls) {
        Fail "No release runtime DLLs found in vcpkg tree: $vcpkgBin"
    }
    foreach ($dll in $vcpkgRuntimeDlls) {
        Copy-Item $dll.FullName $pkgDir -Force
        Write-Host "Copied vcpkg runtime: $($dll.Name)"
    }
} else {
    Fail "-VcpkgInstalledDir is required to deploy transitive runtime DLLs"
}

# --- MSVC runtime -------------------------------------------------------------
# vcpkg's windeployqt copies vc_redist.x64.exe, but the portable package
# contract requires the runtime DLLs beside the executables. The MSVC developer
# environment exposes the matching redistributable root via VCToolsRedistDir.
if (-not $env:VCToolsRedistDir) {
    Fail "VCToolsRedistDir is not set; run from an MSVC developer environment"
}
$msvcCrtDir = Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC143.CRT"
if (-not (Test-Path $msvcCrtDir)) {
    Fail "MSVC runtime directory not found: $msvcCrtDir"
}
# Qt 6 built by the current MSVC toolset imports msvcp140_1.dll and
# msvcp140_2.dll in addition to the traditional three files. Copy the complete
# matching CRT directory to remain correct across runner toolset updates.
$msvcRuntimeDlls = Get-ChildItem -Path $msvcCrtDir -Filter "*.dll" -File
if (-not $msvcRuntimeDlls) {
    Fail "No MSVC runtime DLLs found: $msvcCrtDir"
}
foreach ($dll in $msvcRuntimeDlls) {
    Copy-Item $dll.FullName $pkgDir -Force
    Write-Host "Copied MSVC runtime: $($dll.Name)"
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
# Archive the package contents, not the staging directory itself. The deploy
# checks and end users should see hlm_app.exe at the archive root after unzip.
Compress-Archive -Path (Join-Path $pkgDir "*") -DestinationPath $zipPath
Write-Host "Package created: $zipPath"
exit 0
