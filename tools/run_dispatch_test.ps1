# End to end: a RayQuery COMPUTE shader running on the GTX 1070.
#
# This is the whole project in one test. The application compiles RayQuery,
# creates a compute pipeline and calls Dispatch. None of that can work on Tier
# 1.0 hardware. Through the shim it does:
#
#   CheckFeatureSupport      reports Tier 1.1  (only with DXR_TIER11=1)
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
    @{ name = 'sm66';    pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_sm66.hlsl');
       desc = 'Shader Model 6.6 binding, which is what Unreal compiles its RayQuery shaders at' },
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
       desc = 'triangle-only shader on a scene holding procedural geometry too' },
    # The case the shim could not serve at all until per-index typed records.
    # A shader that COMMITS procedural hits, on a scene that also holds
    # triangles, with the two kinds on different records. Slot 0 gets a
    # TRIANGLES hit group whose any-hit always ignores, so the triangle
    # geometry is traversed with a record of the right type and produces
    # nothing. Measured: with that rejecting record replaced by the real one,
    # this scene gives 15418 hits against WARP's 7396, the 8022 difference
    # being exactly the triangle hits. So this case fails if the typing stops.
    @{ name = 'mixedproc'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc.hlsl', '--mixed', '--contrib');
       desc = 'procedural shader on a mixed scene, triangles on their own record' },
    # The last shader-side gap: ONE Proceed loop that commits BOTH kinds. It
    # lowers to an any-hit AND an intersection shader from the same body, plus
    # two closest-hits, because a triangle hit reports committed status 1 and a
    # procedural one 2. Two REAL hit groups, one per record.
    @{ name = 'both'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_both.hlsl', '--mixed', '--contrib');
       desc = 'a query committing BOTH triangle and procedural hits' },
    # Resource arrays through a descriptor table, now that RunRayQuery honours
    # --table and these can run as compute shaders at all. --table binds the
    # real output at slot 2 and decoys at 0, 1 and 3, so a mishandled index
    # writes nowhere visible and the case reports 0 hits.
    @{ name = 'table'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_table.hlsl', '--table');
       desc = 'resource array at a CONSTANT index, through a descriptor table' },
    # The index is a cbuffer load, so the element is reached by a real
    # getelementptr INSTRUCTION rather than a folded constant expression.
    # Measured, with the index poisoned to a constant 0: 0 hits, not 14450.
    @{ name = 'dyn'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dyn.hlsl', '--table');
       desc = 'DYNAMIC descriptor indexing, the index computed at runtime' },
    # The same through NonUniformResourceIndex, which marks the getelementptr
    # with !dx.nonuniform. The value is wave-uniform so the RESULT is the same;
    # what this checks is that the metadata survives the whole path.
    @{ name = 'dynnu'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynnu.hlsl', '--table');
       desc = 'dynamic index through NonUniformResourceIndex' },
    # The SAME shader, created through CreatePipelineState, the pipeline STREAM
    # form, instead of CreateComputePipelineState.
    #
    # This is not a variation on the lowering, it is a different D3D12 entry
    # point, and it is the one a real engine uses. Unreal creates every PSO
    # through the stream form, and until this case existed the shim's
    # substitution covered only the struct form, so every RayQuery shader in a
    # real game reached the driver unexamined. The driver rejected them, and
    # Unreal treats a failed compute PSO as fatal.
    #
    # The stream the harness builds puts the ROOT SIGNATURE FIRST, deliberately,
    # because that is what an engine does and it is exactly what the first
    # version of the walker could not get past.
    @{ name = 'stream'; pat = 'opaque'; extra = @('--stream');
       desc = 'the stream form of CreatePipelineState, which is what Unreal uses' },
    # GeometryIndex, the accessor this project called impossible for longest,
    # and the one a real Unreal 5.8 run refused 108 shaders on.
    #
    # --geom is one bottom-level structure holding FOUR geometries, a quad per
    # quadrant. Every other scene here has one geometry per structure, so the
    # right answer is 0 everywhere and a lowering that dropped the index would
    # pass. Here the candidate values GATE THE COMMIT: geometry + contribution
    # == 3 is skipped, so without --contrib the missing quadrant is geometry 3
    # and with it geometry 1. The gap MOVES, so neither accessor can be a
    # constant and both have to be right.
    @{ name = 'geom'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom');
       desc = 'GeometryIndex, baked into the hit shader copy its record points at' },
    # RayFlags (195), read BOTH inside the Proceed loop and outside it, which
    # take different routes: dx.op.rayFlags in the generated any-hit, where it
    # is legal, and a folded constant in the raygen, where it is not. A
    # lowering using the constant in both places passes a test that reads it in
    # only one.
    #
    # The flags are a UNION of the template argument and the TraceRayInline
    # argument, 0x002 | 0x200 = 0x202, so dropping either half is visible. The
    # loop read gates the commit, so a wrong value there empties the image.
    @{ name = 'flags'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_flags.hlsl');
       desc = 'RayFlags, folded in the raygen and intrinsic in the any-hit' },
    # Values computed BEFORE the Proceed loop and read INSIDE it: a cbuffer
    # threshold and a value derived from the thread id. The ordinary shape of a
    # real alpha test, and what Unreal refused 59 shaders on.
    #
    # Neither is caller state: a cbuffer is bound by the GLOBAL root signature
    # and the ray index is the same ray, so the generated hit shader rebuilds
    # the chain rather than being refused. The two arrive differently, a
    # cbuffer load and a DispatchRaysIndex, so both are exercised.
    # The same shader with the TraceRayInline flags computed at RUNTIME.
    #
    # Unreal does this in 118 shaders. The lowering used to fold the flags into
    # the TraceRay call, so it could only accept a constant; dx.op.traceRay
    # takes RayFlags as an ordinary i32 operand, so it does not have to.
    # Expected answer identical to the static case, 0x202, which is the point:
    # same result, different route. Dropping the operand loses the runtime half
    # and RayFlags() reports 0x002.
    @{ name = 'dynflags'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_dynflags.hlsl');
       desc = 'ray flags computed at runtime, passed through rather than folded' },
    # A loop body with real CONTROL FLOW, so DXC emits a phi inside the loop,
    # and a [branch] hint, so it emits a distinct metadata node.
    #
    # Both were bugs. A phi writes its predecessors as [ %val, %bb12 ] with
    # no label keyword, so the operand scan counted the predecessor BLOCKS as
    # values and refused 118 of Unreal's shaders for reading them. And the
    # metadata scan did not match distinct, so that id stayed invisible and
    # the fresh-id counter handed it out again.
    @{ name = 'phi'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_phi.hlsl');
       desc = 'a phi in the loop body, and a distinct metadata node beside it' },
    # A phi whose predecessor is the ENTRY BLOCK, which DXC gives no label
    # because nothing can branch to it. Normalisation had nothing to rename, so
    # the phi referenced a block that was never defined and the assembler said
    # "use of undefined value '%bb0'".
    #
    # It survived the whole of Phase 5 because EVERY other case here opens with
    # `if (tid.x >= width) return;`, and that guard puts a block between the
    # entry and everything else. One shared habit across every test, hiding one
    # bug.
    @{ name = 'entryphi'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_entryphi.hlsl');
       desc = 'a phi whose predecessor is the entry block, which has no label' },
    @{ name = 'param'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_param.hlsl');
       desc = 'loop body reads a cbuffer threshold and a ray-index value from outside' },
    @{ name = 'geomcontrib'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib');
       desc = 'the same, with a nonzero contribution, so the record index is contribution + geometry' },
    # The same at Shader Model 6.6, which is what Unreal uses, and where the
    # record handle has to be ANNOTATED: a bare createHandleForLib on a local
    # root signature resource is what crashed the Pascal driver.
    @{ name = 'geomsm66'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom_sm66.hlsl', '--geom', '--contrib');
       desc = 'GeometryIndex and contribution at Shader Model 6.6, record handle annotated' },
    # The record constants are BAKED into a copy of the hit shaders per pair,
    # because a hit shader reading a local root signature crashes the Pascal
    # driver (phase5/cases/driver-crash/). `geom` only copies AnyHit and
    # ClosestHit; this copies all four, and needs a triangle AND a procedural
    # hit group per pair. Every commit is gated on the contribution the record
    # reports, so a record pointing at the wrong copy loses its whole instance:
    # measured, with every record forced onto copy 0, 7396 mismatches, which
    # is exactly the procedural hit count.
    @{ name = 'bothgeom'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_bothgeom.hlsl', '--mixed', '--contrib');
       desc = 'both kinds, each commit gated on its baked contribution, four hit shaders copied per pair' },
    # SV_GroupID, SV_GroupThreadID and SV_GroupIndex, which a raygen does not
    # have; rebuilt from DispatchRaysIndex and numthreads. The pixel comes only
    # from the group values and SV_GroupIndex gates the commit in the any-hit.
    # Measured: swapping the axes' group sizes gives 5999 mismatches, and a
    # wrong row stride in the flattening gives 5398. The first version of the
    # gate could not see the row stride at all and passed with it wrong.
    @{ name = 'group'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_group.hlsl');
       desc = 'SV_GroupID, SV_GroupThreadID and SV_GroupIndex, numthreads(16,8,1)' },
    # ExecuteIndirect with a DISPATCH signature, which is how Unreal issues
    # most of its Lumen and MegaLights inline passes. Until 0.39.0 the shim
    # forwarded it and the GPU ran the do-nothing carrier, silently. The
    # arguments are GPU-written, in a COMBINED read state, at byte 36 among
    # decoys of 3 groups, so an ignored dispatch draws nothing and a wrong
    # offset draws a corner.
    @{ name = 'indirect'; pat = 'alpha'; extra = @('--indirect');
       desc = 'indirect compute dispatch, group counts read back from the GPU' },
    @{ name = 'indirectup'; pat = 'opaque'; extra = @('--indirectup');
       desc = 'indirect compute dispatch from an upload buffer, read at record time' },
    # The record pairs are only known at dispatch, so this one REBAKES inside
    # the queue hook, at submit time, on the thread that submits.
    @{ name = 'indirectgeom'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--indirect');
       desc = 'indirect dispatch of a shader whose hit shaders are baked per record pair' },
    # The top-level structure is rebuilt IN PLACE with new contributions after
    # a first dispatch, as an engine does when a level loads. 0.39.0 read each
    # address once and kept a one-record table: 9248 of 18496 hits, and in
    # Escher's open world a GPU hang. Measured separately: with the re-read
    # poisoned, the table padding alone still matches, since this shader bakes
    # no record data and pads with its own record.
    @{ name = 'rebuild'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_acc.hlsl', '--multi', '--contrib', '--rebuild');
       desc = 'top-level structure rebuilt in place with new contributions between dispatches' },
    # A new record layout after the real dispatch, twelve times, in the SAME
    # list before it is submitted, so the real dispatch's table is evicted from
    # the shim's cache while nothing has run. 0.40.1 kept every evicted table
    # until the pipeline died, about 3 GB in Escher's open world; 0.40.2 reuses
    # and frees them, and must never do so while a list that reads one can
    # still run. The gate below turns that check off and must diverge.
    @{ name = 'churn'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--churn', '12');
       desc = 'twelve more record layouts recorded after the real dispatch, before submitting' },
    # The same, submitted and waited for after every layout, so evicted tables
    # become idle and the reuse path actually runs.
    @{ name = 'churnflush'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--churn', '20', '--churnflush');
       desc = 'twenty more record layouts, each submitted, so shader table buffers are reused' },
    # The scene moves to a new top-level structure with a different layout and
    # the old one is never built again, as Unreal does when its structure
    # outgrows its buffer. 0.40.2 kept the old one live for 64 builds, the two
    # disagreed about records, and the dispatch was refused: 9248 hit/miss
    # mismatches here, two bursts of refusals per Escher session.
    @{ name = 'move'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--move');
       desc = 'top-level structure moved to a new address with a different layout' },
    # The instances in GPU memory, as Unreal writes them, and the scene rebuilt
    # in place with a different layout. The shim reads GPU-written instances
    # a submission late, so at the dispatch it knows only the OLDER build:
    # 0.51.0 drew from that build's table, 9248 hit/miss and 4624 value
    # mismatches, nothing logged. The dispatch now waits for submit and takes
    # the table from exactly the build it traces.
    @{ name = 'stalegpu'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--rebuild', '--gpuinst');
       desc = 'GPU-written instances, scene rebuilt in place with a new layout' },
    # The same, twelve layouts in one list after the real dispatch: each
    # deferred dispatch needs ITS build's instances, not the latest.
    @{ name = 'churngpu'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--churn', '12', '--gpuinst');
       desc = 'GPU-written instances, twelve layouts in one list' },
    @{ name = 'indirectgpuinst'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--rebuild', '--indirect', '--gpuinst');
       desc = 'GPU-written instances, rebuilt in place, indirect dispatch' },
    # The rebuild recorded in ANOTHER list, submitted in the same
    # ExecuteCommandLists call just before the dispatch's: until 0.52.2 its
    # read was stamped only after the whole call, and the dispatch was refused.
    @{ name = 'sameecl'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--rebuild', '--sameecl', '--gpuinst');
       desc = 'rebuild in another list of the same submission, GPU-written instances' },
    @{ name = 'sameeclind'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--rebuild', '--sameecl', '--gpuinst', '--indirect');
       desc = 'the same, indirect dispatch' },
    # A second LIVE top-level structure whose layout conflicts with the real
    # one. Judged over every live structure this is refused; the shim has to
    # resolve the scene the dispatch traces (0.49.0). And the same with the
    # scene taken from the descriptor heap, Unreal's bindless form, the index
    # in the root CBV and the decoy in the neighbouring slots and at t0.
    @{ name = 'decoy'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--decoy');
       desc = 'a second live structure with a conflicting layout, scene resolved through the root SRV' },
    @{ name = 'bindlessrq'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom_bindless_sm66.hlsl', '--geom', '--contrib', '--bindless');
       desc = 'the scene from the descriptor heap, index in the root CBV, conflicting decoy beside it' },
    # A Proceed loop that APPENDS a record per candidate, the MegaLights and
    # Lumen shape. The records land in traversal order, which is undefined, so
    # raytest sorts them into <out>.append and those must be identical too.
    @{ name = 'append'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_append_sm66.hlsl', '--multi', '--append');
       desc = 'a Proceed loop that appends a record per candidate through a UAV counter'; append = $true },
    # Values LOADED before the loop and used inside it, MegaLights' shape: the
    # thread's own output slot (a UAV, prefilled) and a read-only buffer. The
    # UAV value travels in the payload, the SRV is read again in the hit
    # shader. The gate below runs the two sides with different prefill seeds
    # and must diverge, so the pre-read value is shown to drive the image.
    @{ name = 'preload'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_preload_sm66.hlsl', '--prefill', '1');
       desc = 'values loaded before the loop, one carried in the payload, one re-read' },
    # Two RayQuery objects one after the other, the shape of 33 Lumen and
    # Niagara shaders: two traces, one shared any-hit choosing the loop body by
    # a query id in the payload. The bodies commit opposite triangle halves;
    # measured on WARP, swapping them changes 11536 rays, so running the wrong
    # one for either trace cannot match.
    @{ name = 'twoq'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_twoq_sm66.hlsl', '--multi', '--prefill', '1');
       desc = 'two queries in sequence, one any-hit picking the loop body by query id' }
)

$failed = 0
$env:DXR_TIER11 = '1'
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
    $appendOk = $true
    if ($c.append) {
        $appendOk = (Test-Path "$a.append") -and (Test-Path "$b.append") -and
            ((Get-FileHash "$a.append").Hash -eq (Get-FileHash "$b.append").Hash)
        $recs = if (Test-Path "$a.append") { [BitConverter]::ToUInt32([IO.File]::ReadAllBytes("$a.append"), 0) } else { 0 }
        Write-Host "  appended records   : $recs, sorted sets $(if ($appendOk) { 'identical' } else { 'DIFFER' })"
        Remove-Item "$a.append", "$b.append" -ErrorAction SilentlyContinue
    }
    if (($diff -match 'RESULT: MATCH') -and $appendOk) { Write-Host "  $n : PASS" }
    else { Write-Host "  $n : FAIL"; $failed++ }
    Remove-Item $a, $b -ErrorAction SilentlyContinue
}
$env:DXR_TIER11 = ''

# Tier 1.1 is ON by default now: a proxy DLL only sits beside an executable
# because somebody put it there, and that is the opt-in. What has to be proven
# instead is that the OFF switch still works, because it is the only way back
# short of deleting the file, and because it is what a bug report will be asked
# to try first.
#
# A non-zero exit is the EXPECTED outcome in the off cases, so stop 'Stop'
# aborting.
$ErrorActionPreference = 'Continue'

Write-Host ''
Write-Host '=== a shader table still in flight is never reused ==='
# DXR_TIER11_NOHOLD=1 makes the shim treat every evicted table as idle. The
# churn case must then DIVERGE: if it still matched, it could not see a table
# reused too early and its pass above would mean nothing.
$env:DXR_TIER11 = '1'
$x = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--churn', '12')
& .\raytest.exe warp rayquery alpha dp_hold_a.bin @x | Out-Null
$env:DXR_TIER11_NOHOLD = '1'
& .\raytest.exe hw rayquery alpha dp_hold_b.bin @x | Out-Null
$env:DXR_TIER11_NOHOLD = $null
$env:DXR_TIER11 = ''
$diff = & .\raytest.exe diff dp_hold_a.bin dp_hold_b.bin 2>&1
Remove-Item dp_hold_a.bin, dp_hold_b.bin -ErrorAction SilentlyContinue
if ($diff -match 'RESULT: DIVERGE') {
    Write-Host '  diverges with the check off, so the churn case can see it'
} else {
    Write-Host '  NOT SENSITIVE: churn matched with in-flight tables reused'
    $failed++
}

Write-Host ''
Write-Host '=== the value read before the loop drives the image ==='
# The preload case with prefill seed 1 on WARP and seed 2 on the 1070 must
# DIVERGE: if the image did not depend on the pre-read, a lowering that
# carried or re-read the wrong value would pass the case above.
$env:DXR_TIER11 = '1'
$x = @('--cs', 'phase5\cases\rayquery_preload_sm66.hlsl', '--prefill')
& .\raytest.exe warp rayquery alpha dp_pre_a.bin @x 1 | Out-Null
& .\raytest.exe hw rayquery alpha dp_pre_b.bin @x 2 | Out-Null
$env:DXR_TIER11 = ''
$diff = & .\raytest.exe diff dp_pre_a.bin dp_pre_b.bin 2>&1
Remove-Item dp_pre_a.bin, dp_pre_b.bin -ErrorAction SilentlyContinue
if ($diff -match 'RESULT: DIVERGE') {
    Write-Host '  diverges with different prefills, so the preload case can see it'
} else {
    Write-Host '  NOT SENSITIVE: preload matched with different prefilled values'
    $failed++
}

Write-Host ''
Write-Host '=== an EMPTY scene draws all misses ==='
# A top-level structure with no instances, as Unreal's is on the first frame
# of a level. WARP crashes tracing one (divide by zero, RayQuery and TraceRay
# alike), so the oracle is the definition: every ray misses. The output is
# prefilled, so a dispatch that is not drawn leaves the pattern's hits
# behind; until 0.52.2 it was refused and left 49154.
foreach ($m in @(@('--gpuinst'), @())) {
    $out = & .\raytest.exe hw rayquery alpha dp_empty.bin --cs phase5\cases\rayquery_geom.hlsl --geom --contrib --empty --prefill 7 @m 2>&1
    Remove-Item dp_empty.bin -ErrorAction SilentlyContinue
    $what = if ($m.Count) { 'GPU-written instances' } else { 'CPU-visible instances' }
    if ($out -match '65536 rays, 0 hits') {
        Write-Host "  $what : drawn, every ray missed"
    } else {
        Write-Host "  $what : NOT DRAWN or wrong: $(($out | Select-String 'rays,').Line)"
        $failed++
    }
}

Write-Host ''
Write-Host '=== tier 1.1 is on by default ==='
$env:DXR_TIER11 = ''
$out = & .\raytest.exe hw rayquery opaque dp_gate.bin 2>&1
Remove-Item dp_gate.bin -ErrorAction SilentlyContinue
if ($out -match 'needs Tier 1\.1') {
    Write-Host '  DEFAULT BROKEN: RayQuery was refused with no configuration at all'
    $failed++
} else {
    Write-Host '  accepted with no configuration at all, as it must'
}

Write-Host ''
Write-Host '=== DXR_TIER11=0 turns it off ==='
$env:DXR_TIER11 = '0'
$out = & .\raytest.exe hw rayquery opaque dp_gate.bin 2>&1
$env:DXR_TIER11 = ''
Remove-Item dp_gate.bin -ErrorAction SilentlyContinue
if ($out -match 'needs Tier 1\.1') {
    Write-Host '  refused with DXR_TIER11=0, as it must'
} else {
    Write-Host '  OFF SWITCH BROKEN: RayQuery was accepted with DXR_TIER11=0'
    $failed++
}

# And through the file, which is the path a user without a terminal takes. The
# env var is cleared above, so this proves the file alone is enough.
Write-Host ''
Write-Host '=== dxr-tier-11.ini turns it off too ==='
'tier11 = 0' | Set-Content dxr-tier-11.ini -Encoding ascii
$out = & .\raytest.exe hw rayquery opaque dp_gate.bin 2>&1
Remove-Item dxr-tier-11.ini, dp_gate.bin -ErrorAction SilentlyContinue
if ($out -match 'needs Tier 1\.1') {
    Write-Host '  refused with tier11 = 0 in dxr-tier-11.ini, as it must'
} else {
    Write-Host '  INI IGNORED: RayQuery was accepted with tier11 = 0 in dxr-tier-11.ini'
    $failed++
}

$ErrorActionPreference = 'Stop'

# What CANNOT be served, and this proves the refusal covering it is reachable
# rather than dead code. When the application routes both geometry kinds to the
# SAME hit group record, that slot would need a procedural record for the
# procedural geometry and a rejecting triangle record for the triangles. A
# record is one or the other, so there is no table that works and refusing is
# the only honest answer.
Write-Host ''
Write-Host '=== both kinds collapsed onto one hit group record is refused ==='
$log = Join-Path $env:TEMP 'dxr-tier-11-proxy.log'
Remove-Item $log -ErrorAction SilentlyContinue
$env:DXR_TIER11 = '1'
& .\raytest.exe hw rayquery alpha dp_mix.bin --cs phase5\cases\rayquery_proc.hlsl --mixed |
    Out-Null
$env:DXR_TIER11 = ''
Remove-Item dp_mix.bin -ErrorAction SilentlyContinue
if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'routes BOTH triangle and procedural geometry to the same' -Quiet)) {
    Write-Host '  refused, with the reason, as it must'
} else {
    Write-Host '  REFUSAL MISSING: the unservable layout was allowed through'
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
