# GeometryIndex() in an application's own DXR shaders: every gitest.exe
# configuration, each checked against WARP. Build gitest.exe with
# build_tier11.bat and the shim with build_proxy.bat first; both sit in the
# repository root beside DXC.
#
# Exit code: the number of configurations that did not match.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

if (-not (Test-Path '.\gitest.exe') -or -not (Test-Path '.\d3d12.dll')) {
    Write-Host 'gitest.exe or d3d12.dll missing; run build_tier11.bat and build_proxy.bat first.'
    Pop-Location; exit 1
}

$cfgs = @()
# How the scene is bound (global root signature), how the table and
# instances reach the GPU, at 6.5 and 6.6, in one state object, through
# collections, and grown by AddToStateObject.
foreach ($b in @('', '--collections', '--grow')) {
    foreach ($x in @('', '--table', '--indirect', '--indirectgpu', '--gpuinst', '--stale')) {
        foreach ($s in @('', '--sm66')) { $cfgs += ,@(@($b, $x, $s) | Where-Object { $_ }) }
    }
    foreach ($x in @('--bindless', '--bindlessrc')) { $cfgs += ,@(@($b, $x) | Where-Object { $_ }) }
}
$cfgs += ,@('--libassoc')
$cfgs += ,@('--libassoc', '--sm66')
$cfgs += ,@('--libassoc', '--indirectgpu')
$cfgs += ,@('--libassoc', '--stale')
# The scene through the RAYGEN's local root signature (0.55.0), its record in
# CPU or GPU memory, with the arguments in CPU or GPU memory.
foreach ($b in @('', '--collections', '--grow')) {
    foreach ($l in @('--localscene', '--localscenetable')) {
        foreach ($x in @('', '--gpusbt', '--indirect', '--indirectgpu', '--gpuinst', '--stale')) {
            foreach ($s in @('', '--sm66')) { $cfgs += ,@(@($b, $l, $x, $s) | Where-Object { $_ }) }
        }
    }
}
$cfgs += ,@('--localscene', '--indirectgpu', '--gpusbt')
$cfgs += ,@('--localscenetable', '--indirectgpu', '--gpusbt', '--sm66')
$cfgs += ,@('--localscene', '--stale', '--gpusbt')
$cfgs += ,@('--collections', '--localscene', '--indirectgpu', '--gpusbt')

$fail = @()
foreach ($c in $cfgs) {
    & .\gitest.exe $c 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { $fail += ('[' + ($c -join ' ') + ']') }
}
Write-Host "gitest: $($cfgs.Count) configurations, $($cfgs.Count - $fail.Count) match WARP"
foreach ($f in $fail) { Write-Host "  FAILED $f" }
Pop-Location
exit $fail.Count
