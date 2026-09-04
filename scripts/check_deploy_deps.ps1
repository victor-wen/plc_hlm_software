# check_deploy_deps.ps1 - Verify a release package contains every runtime
# dependency required by spec §17 (Task 21).
#
# Checks (missing any -> exit 1 with the list):
#   platforms/qwindows.dll
#   sqldrivers/qsqlite.dll
#   Qt6SerialBus*.dll, Qt6SerialPort*.dll
#   libcrypto-3-x64.dll (OpenSSL Crypto)
#   opencv_core*.dll (OpenCV, HLM_ENABLE_VISION=ON)
#   vcruntime140.dll, vcruntime140_1.dll, msvcp140.dll (MSVC runtime)
#   LICENSES/ present and non-empty
#
# -ExpectMissing <name>: negative test support. If <name> is missing, the
# check PASSES (the expected failure was observed); if <name> is present, the
# check FAILS.
#
# Usage:
#   pwsh -NoProfile -File scripts/check_deploy_deps.ps1 -PackageDir dist/check
#   pwsh -NoProfile -File scripts/check_deploy_deps.ps1 -PackageDir dist/check -ExpectMissing sqldrivers/qsqlite.dll

param(
    [Parameter(Mandatory = $true)]
    [string]$PackageDir,
    [string]$ExpectMissing = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not (Test-Path $PackageDir)) {
    Write-Error "PackageDir not found: $PackageDir"
    exit 1
}

$missing = @()

function Test-Required([string]$relPath) {
    if (-not (Test-Path (Join-Path $PackageDir $relPath))) {
        $script:missing += $relPath
    }
}

# --- Required files ----------------------------------------------------------
Test-Required "platforms/qwindows.dll"
Test-Required "sqldrivers/qsqlite.dll"

$serialBus = Get-ChildItem -Path $PackageDir -Filter "Qt6SerialBus*.dll" -ErrorAction SilentlyContinue
if (-not $serialBus) { $missing += "Qt6SerialBus*.dll" }
$serialPort = Get-ChildItem -Path $PackageDir -Filter "Qt6SerialPort*.dll" -ErrorAction SilentlyContinue
if (-not $serialPort) { $missing += "Qt6SerialPort*.dll" }

Test-Required "libcrypto-3-x64.dll"

$opencv = Get-ChildItem -Path $PackageDir -Filter "opencv_core*.dll" -ErrorAction SilentlyContinue
if (-not $opencv) { $missing += "opencv_core*.dll" }

Test-Required "vcruntime140.dll"
Test-Required "vcruntime140_1.dll"
Test-Required "msvcp140.dll"

$licensesDir = Join-Path $PackageDir "LICENSES"
if (-not (Test-Path $licensesDir)) {
    $missing += "LICENSES/"
} elseif ((Get-ChildItem $licensesDir -File | Measure-Object).Count -eq 0) {
    $missing += "LICENSES/ (empty)"
}

# --- Negative test support ---------------------------------------------------
if ($ExpectMissing -ne "") {
    if (Test-Path (Join-Path $PackageDir $ExpectMissing)) {
        Write-Error "Negative test failed: expected '$ExpectMissing' to be missing but it is present"
        exit 1
    }
    Write-Host "Negative test passed: '$ExpectMissing' is missing as expected"
    exit 0
}

# --- Report ------------------------------------------------------------------
if ($missing.Count -gt 0) {
    # Report the complete list before returning failure. Write-Error would
    # terminate immediately under ErrorActionPreference=Stop.
    Write-Host "Deploy dependency check FAILED. Missing:"
    foreach ($m in $missing) { Write-Host "  - $m" }
    exit 1
}

Write-Host "Deploy dependency check PASSED: all required DLLs, plugins and LICENSES/ present."
exit 0
