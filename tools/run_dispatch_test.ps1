# End to end: a RayQuery COMPUTE shader running on the GTX 1070.
#
# This is the whole project in one test. The application compiles RayQuery,
# creates a compute pipeline and calls Dispatch. None of that can work on Tier
# 1.0 hardware. Through the shim it does:
#
#   CheckFeatureSupport      reports Tier 1.1  (only with DXR11_TIER11=1)
#   CreateComputePipelineState  lowers the DXIL, builds a state object and
#                               shader table, returns a stand-in
#   SetPipelineState         recognises the stand-in
#   Dispatch                 becomes DispatchRays, groups times numthreads
#
# WARP running the same shader natively is the oracle, every run.
#
#   .\tools\run_dispatch_test.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

if (-not (Test-Path '.\d3d12.dll')) {
    Write-Host 'd3d12.dll missing; run build_proxy.bat first.'
    Pop-Location; exit 1
}
foreach ($dll in @('dxcompiler.dll', 'dxil.dll')) {
    if (-not (Test-Path ".\$dll")) {
        Write-Host "$dll must sit beside the shim; the rewriter loads it by full path."
        Pop-Location; exit 1
    }
}

# pattern is what raytest builds the scene for; cs picks the shader.
$cases = @(
    @{ name = 'opaque';  pat = 'opaque'; extra = @();
       desc = 'built-in opaque RayQuery, numthreads(8,8,1)' },
    @{ name = 'alpha';   pat = 'alpha';  extra = @();
       desc = 'built-in alpha-tested RayQuery, generated any-hit' },
    @{ name = 'indep';   pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_indep.hlsl');
       desc = 'independent shader, resource read in the Proceed loop, numthreads(16,16,1)' },
    @{ name = 'proc';    pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_proc.hlsl', '--proc');
       desc = 'procedural primitives, generated intersection shader' },
    @{ name = 'abort';   pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_abort.hlsl', '--multi');
       desc = 'Abort(), order-independent observable only' },
    @{ name = 'acc';     pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_acc.hlsl', '--multi');
       desc = 'the 11 accessors from the Unreal survey, end to end' },
    @{ name = 'ids';     pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_ids.hlsl', '--multi');
       desc = 'CommittedInstanceIndex and CommittedPrimitiveIndex' },
    # The instances carry DIFFERENT hit group contributions, so the shader
    # table has to be sized to the scene rather than to one record. Measured:
    # with a single record this scene loses exactly the hits belonging to the
    # instance contributing 1, 9248 of 18496, silently. So this case fails if
    # the table stops growing.
    @{ name = 'contrib'; pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_acc.hlsl', '--multi', '--contrib');
       desc = 'nonzero InstanceContributionToHitGroupIndex, table sized to the scene' },
    # A triangle-only shader on a scene that ALSO holds procedural geometry.
    # The shim's hit group is triangles-only and the procedural geometry simply
    # reports nothing, which is the same answer Tier 1.1 gives, since the
    # shader never commits a procedural candidate either. Measured, not assumed:
    # the debug layer is silent on this and on the case that IS wrong, so it
    # settles nothing.
    @{ name = 'mixedtri'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_acc.hlsl', '--mixed');
       desc = 'triangle-only shader on a scene holding procedural geometry too' }
)

$failed = 0
$env:DXR11_TIER11 = '1'
foreach ($c in $cases) {
    $n = $c.name
    Write-Host ''
    Write-Host "=== $n : $($c.desc) ==="
    $a = "dp_${n}_a.bin"; $b = "dp_${n}_b.bin"

    # WARP runs RayQuery natively; the 1070 runs it only because of the shim.
    & .\raytest.exe warp rayquery $c.pat $a @($c.extra) | Out-Null
    & .\raytest.exe hw   rayquery $c.pat $b @($c.extra) | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  the hardware run FAILED; the dispatch path did not take over'
        $failed++
        Remove-Item $a, $b -ErrorAction SilentlyContinue
        continue
    }
    $diff = & .\raytest.exe diff $a $b 2>&1
    $diff | Where-Object { $_ -match 'rays|mismatches|max|RESULT' } |
        ForEach-Object { "  $($_.Trim())" }
    if ($diff -match 'RESULT: MATCH') { Write-Host "  $n : PASS" }
    else { Write-Host "  $n : FAIL"; $failed++ }
    Remove-Item $a, $b -ErrorAction SilentlyContinue
}
$env:DXR11_TIER11 = ''

# The gate matters as much as the feature. Without the environment variable the
# shim must still report Tier 1.0, so an application cannot be tempted into
# emitting RayQuery before this path is trusted.
Write-Host ''
Write-Host '=== the tier flip stays OFF by default ==='
# A non-zero exit is the EXPECTED outcome here, so stop 'Stop' aborting.
$ErrorActionPreference = 'Continue'
$out = & .\raytest.exe hw rayquery opaque dp_gate.bin 2>&1
$ErrorActionPreference = 'Stop'
Remove-Item dp_gate.bin -ErrorAction SilentlyContinue
if ($out -match 'needs Tier 1\.1') {
    Write-Host '  refused without DXR11_TIER11, as it must'
} else {
    Write-Host '  GATE BROKEN: RayQuery was accepted without the opt-in'
    $failed++
}

# The other direction of the same mismatch is NOT safe, and this proves the
# refusal that covers it is reachable rather than dead code. A shader that
# commits procedural hits, on a scene that also holds triangles, measured 15418
# hits against WARP's 7396: the 8022 difference is exactly the triangle hits,
# committed by a closest-hit that labels everything procedural.
Write-Host ''
Write-Host '=== a procedural shader on a scene with triangles is refused ==='
$log = Join-Path $env:TEMP 'dxr11_proxy.log'
Remove-Item $log -ErrorAction SilentlyContinue
$env:DXR11_TIER11 = '1'
& .\raytest.exe hw rayquery alpha dp_mix.bin --cs phase5\cases\rayquery_proc.hlsl --mixed |
    Out-Null
$env:DXR11_TIER11 = ''
Remove-Item dp_mix.bin -ErrorAction SilentlyContinue
if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'commits procedural hits, and the scene also' -Quiet)) {
    Write-Host '  refused, with the reason, as it must'
} else {
    Write-Host '  REFUSAL MISSING: the wrong-geometry case was allowed through'
    $failed++
}

Write-Host ''
if ($failed -eq 0) {
    Write-Host 'DISPATCH PATH: RayQuery compute shaders run on the 1070 and match WARP.'
} else {
    Write-Host "DISPATCH PATH: $failed check(s) failed."
}
Pop-Location
exit $failed
