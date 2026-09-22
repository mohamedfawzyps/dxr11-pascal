# Assembles the release zip.
#
# A script rather than a list in someone's notes, so what ships is recorded in
# the repository and is the same every time.
#
# What is deliberately NOT in it:
#
#   dxcompiler.dll, dxil.dll   Microsoft's. The project redistributes nothing
#                              that is not its own, and dxil.dll in particular
#                              is a signed binary with its own terms. The
#                              readme tells the user where to get them.
#   docs, source, tests        already on the project page, and a user
#                              installing a DLL does not want them
#   .lib .exp .pdb             build leftovers
#
#   powershell -ExecutionPolicy Bypass -File tools\make-release.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

# The version comes from the header the DLL was built from, so the zip cannot
# disagree with what the binary logs about itself.
$verLine = Select-String -Path (Join-Path $root 'proxy\version.h') -Pattern '#define DXR_TIER11_VERSION "([^"]+)"'
if (-not $verLine) { throw 'Cannot read the version from proxy/version.h' }
$version = $verLine.Matches[0].Groups[1].Value

$name = "pascal-dxr-tier-1.1-v$version"
$stage = Join-Path $root "release\$name"
$zip = Join-Path $root "release\$name.zip"

if (Test-Path (Join-Path $root 'release')) { Remove-Item (Join-Path $root 'release') -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

# The shim, and nothing else that is a build artifact.
$dll = Join-Path $root 'd3d12.dll'
if (-not (Test-Path $dll)) { throw 'd3d12.dll not found. Run build_proxy.bat first.' }
Copy-Item $dll $stage

Copy-Item (Join-Path $root 'dxr-tier-11.example.ini') $stage
Copy-Item (Join-Path $root 'tools\dxr-tier-11-setup.ps1') $stage
Copy-Item (Join-Path $root 'tools\dxr-tier-11-setup.bat') $stage

# Flat layout, so the setup script finds d3d12.dll beside itself.
$readme = Get-Content (Join-Path $root 'tools\release-readme.txt') -Raw
$readme.Replace('{VERSION}', $version) |
    Set-Content (Join-Path $stage 'README.txt') -Encoding ascii

$license = Join-Path $root 'LICENSE'
if (Test-Path $license) {
    Copy-Item $license (Join-Path $stage 'LICENSE.txt')
} else {
    Write-Host ''
    Write-Host '  WARNING: no LICENSE file in the repository, so none is in the zip.'
    Write-Host '  Without one, the default is all rights reserved: nobody is'
    Write-Host '  permitted to use what you are about to publish.'
    Write-Host ''
}

Compress-Archive -Path $stage -DestinationPath $zip -Force

Write-Host "Built $zip"
Write-Host ''
Get-ChildItem $stage | ForEach-Object {
    Write-Host ("  {0,-30} {1,8:N0} bytes" -f $_.Name, $_.Length)
}
Write-Host ''
Write-Host "Version $version, from proxy\version.h."
Write-Host 'Check it matches the tag you are releasing.'
