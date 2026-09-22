# Build both proxies and install them, in one step.
#
# This exists because of a wasted run. 0.26.0 fixed three refusal classes, the
# version was bumped and dxrw.exe rebuilt, but the proxy DLL itself was not, so
# the game ran 0.25.0 and reported the same failures. The dxgi.dll beside it was
# older still, at 0.24.0.
#
# The evidence was there: the log names its own version on the first line of
# every run. But a check that depends on somebody reading a log is not a check,
# which is the same reason the shader dump needed a checkbox and the release
# script needed a tag gate.
#
#   powershell -ExecutionPolicy Bypass -File tools\install.ps1 -To "C:\Path\To\Game\Binaries\Win64"
#
# -NoDxgi skips the device id spoof, which only Unreal needs.

param(
    [Parameter(Mandatory = $true)][string]$To,
    [switch]$NoDxgi
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path -PathType Container $To)) {
    throw "Not a directory: $To"
}

$verLine = Select-String -Path (Join-Path $root 'proxy\version.h') `
    -Pattern '#define\s+DXR_TIER11_VERSION\s+"([^"]+)"'
if (-not $verLine) { throw 'Cannot read the version from proxy/version.h' }
$version = $verLine.Matches[0].Groups[1].Value

$names = @('d3d12.dll')
if (-not $NoDxgi) { $names += 'dxgi.dll' }

Write-Host "Building $version" -ForegroundColor Cyan
& (Join-Path $root 'build_proxy.bat') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'build_proxy.bat failed' }
if (-not $NoDxgi) {
    & (Join-Path $root 'build_dxgi.bat') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'build_dxgi.bat failed' }
}

foreach ($n in $names) {
    $src = Join-Path $root $n
    if (-not (Test-Path $src)) { throw "$n was not built" }
    $built = (Get-Item $src).VersionInfo.FileVersion
    if ($built -notlike "$version*") {
        throw "$n reports $built but proxy/version.h says $version. The build did not pick up the header."
    }
    Copy-Item $src $To -Force
}

# Verify what LANDED, not what was copied. A locked file, a redirected folder or
# an antivirus quarantine all look like a successful Copy-Item.
Write-Host ""
$bad = @()
foreach ($n in $names) {
    $dst = Join-Path $To $n
    if (-not (Test-Path $dst)) { $bad += "$n is not there after copying"; continue }
    $sameBytes = (Get-FileHash (Join-Path $root $n)).Hash -eq (Get-FileHash $dst).Hash
    $v = (Get-Item $dst).VersionInfo.FileVersion
    Write-Host ("  {0,-12} {1,-8} {2}" -f $n, $v, $(if ($sameBytes) { 'matches the build' } else { 'DIFFERENT BYTES' }))
    if (-not $sameBytes) { $bad += "$n on disk is not the file just built" }
}
if ($bad) { throw ($bad -join "; ") }

foreach ($n in @('dxcompiler.dll', 'dxil.dll')) {
    if (-not (Test-Path (Join-Path $To $n))) {
        Write-Host "  $n is NOT there. Without it the shim stands aside entirely and does nothing." -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Installed $version to $To" -ForegroundColor Green
Write-Host "The log's first line names the version that actually ran. Check it matches."