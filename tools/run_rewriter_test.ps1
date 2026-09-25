# End to end regression for the automated RayQuery rewriter.
#
# For each pattern: lower the RayQuery compute shader with dxrewrite, assemble
# and sign it with dxilrt, run it on the GTX 1070, and diff against WARP's
# native Tier 1.1 RayQuery as ground truth.
#
#   .\tools\run_rewriter_test.ps1
#
# Ground truth comes from WARP every run rather than from a stored baseline,
# so a change to the scene or the harness cannot quietly invalidate it.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

# Each case: the .ll to lower, the raytest pattern to diff against, and any
# extra raytest flags the shader's bindings need.
$cases = @(
    @{ name = 'opaque'; src = 'phase5\dxil\rayquery_opaque.ll'; pat = 'opaque'; flags = @();
       desc = 'pattern 1, opaque closest hit' },
    @{ name = 'alpha';  src = 'phase5\dxil\rayquery_alpha.ll';  pat = 'alpha';  flags = @();
       desc = 'pattern 3, alpha-tested, generated any-hit' },
    @{ name = 'table';  src = 'phase5\cases\rayquery_table.ll'; pat = 'opaque'; flags = @('--table');
       desc = 'resource array indexed at [2], bound through a descriptor table' },
    @{ name = 'tablers'; src = 'phase5\cases\rayquery_tablers.ll'; pat = 'opaque'; flags = @();
       desc = 'descriptor-table root signature, no array; DXIL body is identical' },
    @{ name = 'indep'; src = 'phase5\cases\rayquery_indep.ll'; pat = 'alpha'; flags = @();
       gt = @('--cs', 'phase5\cases\rayquery_indep.hlsl');
       desc = 'independently written: resource READ INSIDE the Proceed loop' },
    @{ name = 'sm66'; src = 'phase5\cases\rayquery_sm66.ll'; pat = 'alpha'; flags = @();
       gt = @('--cs', 'phase5\cases\rayquery_sm66.hlsl');
       desc = 'Shader Model 6.6 resource binding, in the raygen AND the any-hit' },
    @{ name = 'proc'; src = 'phase5\cases\rayquery_proc.ll'; pat = 'alpha'; flags = @('--proc');
       gt = @('--cs', 'phase5\cases\rayquery_proc.hlsl', '--proc');
       desc = 'procedural primitives, generated INTERSECTION shader' },
    @{ name = 'abort'; src = 'phase5\cases\rayquery_abort.ll'; pat = 'alpha'; flags = @('--multi');
       gt = @('--cs', 'phase5\cases\rayquery_abort.hlsl', '--multi');
       desc = 'Abort(), observing only the order-INDEPENDENT hit flag' },
    @{ name = 'acc'; src = 'phase5\cases\rayquery_acc.ll'; pat = 'alpha'; flags = @('--multi');
       gt = @('--cs', 'phase5\cases\rayquery_acc.hlsl', '--multi');
       desc = 'the 11 accessors added after surveying Unreal' },
    @{ name = 'ids'; src = 'phase5\cases\rayquery_ids.ll'; pat = 'opaque'; flags = @('--multi');
       gt = @('--cs', 'phase5\cases\rayquery_ids.hlsl', '--multi');
       desc = 'CommittedInstanceIndex and CommittedPrimitiveIndex, on a scene where both vary' },
    # ONE loop body lowered TWICE, into an any-hit and an intersection shader,
    # plus two closest-hits because the committed status differs. --both makes
    # the harness build the two hit groups and a two-record hit table, and
    # --mixed --contrib builds the scene that needs them: a triangle instance
    # on record 0 and a procedural one on record 1.
    @{ name = 'both'; src = 'phase5\cases\rayquery_both.ll'; pat = 'alpha';
       flags = @('--mixed', '--contrib', '--both');
       gt = @('--cs', 'phase5\cases\rayquery_both.hlsl', '--mixed', '--contrib');
       desc = 'commits BOTH kinds: two hit groups from one Proceed loop' },
    # DYNAMIC descriptor indexing: the array index is a cbuffer load, so the
    # element is reached by a real getelementptr INSTRUCTION rather than a
    # folded constant expression. Ground truth is the built-in shader, as for
    # the `table` case, because --table binds the real output at slot 2 and
    # decoys elsewhere: a mishandled index writes nowhere visible. Measured,
    # with the index poisoned to a constant 0: 0 hits instead of 14450.
    @{ name = 'dyn'; src = 'phase5\cases\rayquery_dyn.ll'; pat = 'opaque';
       flags = @('--table');
       desc = 'resource array indexed by a value the compiler cannot fold' },
    # The same, through NonUniformResourceIndex, which marks the getelementptr
    # with !dx.nonuniform. The index happens to be wave-uniform so the RESULT
    # is the same; what this checks is that the metadata survives and the
    # module still validates and signs.
    @{ name = 'dynnu'; src = 'phase5\cases\rayquery_dynnu.ll'; pat = 'opaque';
       flags = @('--table');
       desc = 'dynamic index through NonUniformResourceIndex' },
    # The thread group ids, rebuilt from DispatchRaysIndex and numthreads in
    # the raygen AND in the generated any-hit. The TraceRay side launches one
    # ray per pixel, which is exactly the thread grid, so the same library is
    # right here as through the proxy.
    @{ name = 'group'; src = 'phase5\cases\rayquery_group.ll'; pat = 'alpha'; flags = @();
       gt = @('--cs', 'phase5\cases\rayquery_group.hlsl');
       desc = 'SV_GroupID, SV_GroupThreadID and SV_GroupIndex, numthreads(16,8,1)' }
)

if (-not (Test-Path 'phase5\dxil\rayquery_opaque.ll')) {
    Write-Host 'phase5\dxil is empty; run build_phase5.bat first.'
    Pop-Location; exit 1
}
if (-not (Test-Path 'phase5out\dxilrt.exe')) {
    Write-Host 'phase5out\dxilrt.exe missing; run build_phase5_tool.bat first.'
    Pop-Location; exit 1
}

New-Item -ItemType Directory -Force 'phase5\out' | Out-Null
$failed = 0

foreach ($c in $cases) {
    $n = $c.name
    Write-Host ''
    Write-Host "=== $n : $($c.desc) ==="
    if (-not (Test-Path $c.src)) {
        Write-Host "  SKIPPED, $($c.src) missing"
        $failed++; continue
    }

    & python phase5\rewriter\dxrewrite.py lower $c.src "phase5\out\$n.ll"
    if ($LASTEXITCODE -ne 0) { $failed++; continue }

    $asm = & .\phase5out\dxilrt.exe asm "phase5\out\$n.ll" "phase5\out\$n.dxil" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  ASSEMBLE/VALIDATE FAILED'
        $asm | ForEach-Object { "    $_" }
        $failed++; continue
    }
    Write-Host '  assembled, validated and signed'

    # The C++ port's correctness bar: byte-identical output to the Python
    # original. Far stronger than "it renders correctly", and it is what keeps
    # the two implementations from drifting apart.
    if (Test-Path '.\phase5out\dxrw.exe') {
        New-Item -ItemType Directory -Force 'phase5\outcpp' | Out-Null
        & .\phase5out\dxrw.exe lower $c.src "phase5\outcpp\$n.ll" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host '  C++ PORT REFUSED what Python lowered'
            $failed++
        } else {
            $a = [IO.File]::ReadAllBytes("phase5\out\$n.ll")
            $b = [IO.File]::ReadAllBytes("phase5\outcpp\$n.ll")
            $same = $a.Length -eq $b.Length
            if ($same) {
                for ($i = 0; $i -lt $a.Length; $i++) {
                    if ($a[$i] -ne $b[$i]) { $same = $false; break }
                }
            }
            if ($same) {
                Write-Host "  C++ port: byte-identical ($($a.Length) bytes)"
            } else {
                Write-Host '  C++ port: DIFFERS from the Python original'
                $failed++
            }
        }
    } else {
        Write-Host '  C++ port: phase5out\dxrw.exe missing, run build_rewriter.bat'
    }

    # WARP RayQuery is the oracle; the 1070 runs what the rewriter produced.
    $gt = if ($c.ContainsKey('gt')) { $c.gt } else { @() }
    & .\raytest.exe warp rayquery $c.pat "rw_${n}_a.bin" @($gt) | Out-Null
    & .\raytest.exe hw traceray $c.pat "rw_${n}_b.bin" --lib "phase5\out\$n.dxil" @($c.flags) | Out-Null
    # The path the PROXY will take: container in, signed container out, with
    # DXC doing the text conversion at both ends. Everything above works on
    # .ll files, which is not what D3D12 ever hands over.
    $srcDxil = [IO.Path]::ChangeExtension($c.src, '.dxil')
    if ((Test-Path '.\phase5out\dxrw.exe') -and (Test-Path $srcDxil)) {
        & .\phase5out\dxrw.exe rewrite $srcDxil "phase5\outcpp\$n.dxil" | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host '  container path: REFUSED'
            $failed++
        } else {
            & .\raytest.exe hw traceray $c.pat "rw_${n}_c.bin" --lib "phase5\outcpp\$n.dxil" @($c.flags) | Out-Null
            $dc = & .\raytest.exe diff "rw_${n}_a.bin" "rw_${n}_c.bin" 2>&1
            if ($dc -match 'RESULT: MATCH') {
                Write-Host '  container path (C++ + DXC): MATCH'
            } else {
                Write-Host '  container path (C++ + DXC): DIVERGE'
                $failed++
            }
            Remove-Item "rw_${n}_c.bin" -ErrorAction SilentlyContinue
        }
    }

    $diff = & .\raytest.exe diff "rw_${n}_a.bin" "rw_${n}_b.bin" 2>&1
    $diff | Where-Object { $_ -match 'rays|mismatches|max|RESULT' } |
        ForEach-Object { "  $($_.Trim())" }
    if ($diff -match 'RESULT: MATCH') {
        Write-Host "  $n : PASS"
    } else {
        Write-Host "  $n : FAIL"
        $failed++
    }
    Remove-Item "rw_${n}_a.bin", "rw_${n}_b.bin" -ErrorAction SilentlyContinue
}

# Record data through a LOCAL ROOT SRV. A hit shader reading a CBUFFER through
# the local root signature crashes the Pascal driver inside CreateStateObject
# (phase5/cases/driver-crash/, 9 of 15 cold compiles); the same library
# reading through a local root SRV was 0 of 30. So GeometryIndex and
# InstanceContributionToHitGroupIndex are read with rawBufferLoad from a raw
# buffer at t0, space1.
#
# Three checks per case: the Python and the C++ lower byte-identically, the
# result validates, and every record read is a rawBufferLoad with NO cbuffer
# load of the record left, which is the property the change exists for.
# Rendering is the dispatch suite's job, because only there does the shim
# build records from a real scene.
Write-Host ''
Write-Host '=== record data through a local root SRV ==='
$recs = @(
    @{ name = 'geom';     src = 'phase5\cases\rayquery_geom.ll' },
    @{ name = 'bothgeom'; src = 'phase5\cases\rayquery_bothgeom.ll' },
    @{ name = 'geomsm66'; src = 'phase5\cases\rayquery_geom_sm66.ll' }
)
foreach ($r in $recs) {
    $n = "rec_$($r.name)"
    if (-not (Test-Path $r.src)) { Write-Host "  $n : SKIPPED, $($r.src) missing"; $failed++; continue }
    & python phase5\rewriter\dxrewrite.py lower $r.src "phase5\out\$n.ll" | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "  $n : Python lower REFUSED"; $failed++; continue }
    New-Item -ItemType Directory -Force 'phase5\outcpp' | Out-Null
    & .\phase5out\dxrw.exe lower $r.src "phase5\outcpp\$n.ll" | Out-Null
    $py = [IO.File]::ReadAllBytes((Resolve-Path "phase5\out\$n.ll"))
    $cp = [IO.File]::ReadAllBytes((Resolve-Path "phase5\outcpp\$n.ll"))
    $same = ($py.Length -eq $cp.Length) -and (-not (Compare-Object $py $cp -SyncWindow 0))
    $asm = & .\phase5out\dxilrt.exe asm "phase5\out\$n.ll" "phase5\out\$n.dxil" 2>&1
    $valid = $LASTEXITCODE -eq 0
    $raw = @(Select-String -Path "phase5\out\$n.ll" -Pattern 'rawBufferLoad\.i32\(i32 139, %dx\.types\.Handle %rq\.cbh').Count
    $cb = @(Select-String -Path "phase5\out\$n.ll" -Pattern 'cbufferLoadLegacy[^\n]*%rq\.cbh').Count
    # At 6.6 every record read must go through an ANNOTATED handle: a bare
    # createHandleForLib on a local root signature resource crashes the driver.
    $ann = @(Select-String -Path "phase5\out\$n.ll" -Pattern 'annotateHandle\(i32 216, %dx\.types\.Handle %rq\.cbh\w+\.lib, %dx\.types\.ResourceProperties \{ i32 11, i32 0 \}').Count
    $is66 = @(Select-String -Path $r.src -Pattern '!"cs", i32 6, i32 6').Count -gt 0
    if ($is66 -and $ann -ne $raw) { $raw = 0 }
    if ($same -and $valid -and $raw -gt 0 -and $cb -eq 0) {
        Write-Host "  $n : PASS ($raw record reads, all rawBufferLoad$(if ($is66) { ', all annotated' }), byte-identical, validates)"
    } else {
        Write-Host "  $n : FAIL (identical=$same validates=$valid raw reads=$raw cbuffer reads=$cb)"
        if (-not $valid) { $asm | Select-Object -Last 4 | ForEach-Object { "    $_" } }
        $failed++
    }
}

Write-Host ''
Write-Host '=== a Proceed loop that appends, lowered into the any-hit ==='
# 0.41.0: an append, a counter update and a store at the index it returned,
# is transplanted rather than refused. Byte-identical between the Python and
# the C++, valid, and the counter update must land in the generated AnyHit.
$src = 'phase5\cases\rayquery_append_sm66.ll'
if (-not (Test-Path $src)) { Write-Host "  append : SKIPPED, $src missing"; $failed++ }
else {
    & python phase5\rewriter\dxrewrite.py lower $src 'phase5\out\append.ll' | Out-Null
    $pyOk = $LASTEXITCODE -eq 0
    New-Item -ItemType Directory -Force 'phase5\outcpp' | Out-Null
    & .\phase5out\dxrw.exe lower $src 'phase5\outcpp\append.ll' | Out-Null
    $same = $pyOk -and ((Get-FileHash 'phase5\out\append.ll').Hash -eq (Get-FileHash 'phase5\outcpp\append.ll').Hash)
    $asm = & .\phase5out\dxilrt.exe asm 'phase5\out\append.ll' 'phase5\out\append.dxil' 2>&1
    $valid = $LASTEXITCODE -eq 0
    $text = if ($pyOk) { Get-Content 'phase5\out\append.ll' -Raw } else { '' }
    $m = [regex]::Match($text, '(?s)define void @AnyHit\(.*?\n\}')
    $inAnyHit = $m.Success -and $m.Value.Contains('@dx.op.bufferUpdateCounter(i32 70')
    if ($same -and $valid -and $inAnyHit) {
        Write-Host '  append : PASS (counter update in AnyHit, byte-identical, validates)'
    } else {
        Write-Host "  append : FAIL (lowered=$pyOk identical=$same validates=$valid inAnyHit=$inAnyHit)"
        if (-not $valid) { $asm | Select-Object -Last 4 | ForEach-Object { "    $_" } }
        $failed++
    }
}

Write-Host ''
Write-Host '=== NVAPI cluster ID calls, on the driver with the slot registered ==='
# 0.41.1. Unreal registers the NVAPI extension UAV (u0 space1001) around the
# compute pipelines it creates, so the shim's CreateStateObject runs with it
# registered and the driver reads stores to that UAV as intrinsics. 0.41.0
# moved a RayQuery cluster-ID call into the any-hit and the driver died
# compiling it. sotest registers the slot the same way (SOTEST_NVEXT). The
# folded library must build; the same case lowered WITHOUT the fold is the
# control and must kill the driver, or this check proves nothing. A crash is
# never cached, so the control stays sensitive on a warm cache.
$case = 'phase5\cases\rayquery_nvapi_sm66'
if (-not (Test-Path "$case.dxil")) { Write-Host "  nvapi : SKIPPED, $case.dxil missing"; $failed++ }
else {
    & 'C:\DW\DXC\bin\x64\dxc.exe' -T rootsig_1_1 -E RS -Fo 'phase5\out\nvapi.rs.bin' "$case.hlsl" | Out-Null
    Copy-Item 'phase5\out\nvapi.rs.bin' 'phase5\out\nvctl.rs.bin'
    $shape = "anyhit=1 intersection=0 both=0 recordconstants=0 baked=0 recordsrv=0`n"
    [IO.File]::WriteAllText("$PWD\phase5\out\nvapi.shape.txt", $shape)
    [IO.File]::WriteAllText("$PWD\phase5\out\nvctl.shape.txt", $shape)
    & .\phase5out\dxrw.exe rewrite "$case.dxil" 'phase5\out\nvapi.out.dxil' | Out-Null
    & python phase5\rewriter\nofold.py "$case.ll" 'phase5\out\nvctl.ll'
    & .\phase5out\dxilrt.exe asm 'phase5\out\nvctl.ll' 'phase5\out\nvctl.out.dxil' | Out-Null
    $env:SOTEST_NVEXT = '0,1001'
    $fix = (& .\phase5out\sotest.exe 'phase5\out\nvapi.out.dxil' 'phase5\out\nvapi.rs.bin' hw 2>&1) -join "`n"
    $ctl = (& .\phase5out\sotest.exe 'phase5\out\nvctl.out.dxil' 'phase5\out\nvctl.rs.bin' hw 2>&1) -join "`n"
    Remove-Item Env:\SOTEST_NVEXT
    $slot = $fix -match 'NVAPI extension slot u0 space1001: init 0, set 0'
    $fixOk = $fix -match 'CreateStateObject\s+hr=0x00000000'
    $ctlDied = $ctl -match 'DXGI_ERROR_DRIVER_INTERNAL_ERROR'
    if ($slot -and $fixOk -and $ctlDied) {
        Write-Host '  nvapi : PASS (folded library builds with the slot registered; unfolded control kills the driver)'
    } else {
        Write-Host "  nvapi : FAIL (slot registered=$slot folded builds=$fixOk control died=$ctlDied)"
        $failed++
    }
}

Write-Host ''
Write-Host '=== GeometryIndex() in an application library: Python and C++ identical, output valid ==='
# Tier 1.1 completion, item a. The render check is gitest.exe (build_tier11.bat),
# which drives a whole DXR 1.0 application through the shim against WARP.
foreach ($sm in @('lib_6_5', 'lib_6_6')) {
    $ll = "phase5\out\geomidx_$sm.ll"
    & 'C:\DW\DXC\bin\x64\dxc.exe' -T $sm -D R_VAL=0 -D M_VAL=1 -D ANYHIT=1 -Fc $ll 'phase5\cases\geomidx_app.hlsl' | Out-Null
    & python phase5\rewriter\geomidx.py $ll "phase5\out\geomidx_$sm.py.ll" | Out-Null
    & .\phase5out\dxrw.exe geomidx $ll "phase5\out\geomidx_$sm.cpp.ll" | Out-Null
    $same = (Get-FileHash "phase5\out\geomidx_$sm.py.ll").Hash -eq (Get-FileHash "phase5\out\geomidx_$sm.cpp.ll").Hash
    $asm = (& .\phase5out\dxilrt.exe asm "phase5\out\geomidx_$sm.py.ll" "phase5\out\geomidx_$sm.dxil" 2>&1) -join "`n"
    $valid = $asm -match 'validated and signed ok'
    if ($same -and $valid) { Write-Host "  geomidx $sm : PASS (byte-identical, validates and signs)" }
    else { Write-Host "  geomidx $sm : FAIL (identical=$same valid=$valid)"; $failed++ }
    # The variant's raygen, pointed at the shim scene (shimtrace), after geomidx,
    # as the shim chains them, on a multiplier-0 library.
    $z = "phase5\out\geomidx0_$sm.ll"
    & 'C:\DW\DXC\bin\x64\dxc.exe' -T $sm -D R_VAL=0 -D M_VAL=0 -D ANYHIT=1 -Fc $z 'phase5\cases\geomidx_app.hlsl' | Out-Null
    & python phase5\rewriter\geomidx.py $z "phase5\out\geomidx0_$sm.gi.ll" | Out-Null
    & python phase5\rewriter\shimtrace.py "phase5\out\geomidx0_$sm.gi.ll" "phase5\out\shimtrace_$sm.py.ll" 0,0 | Out-Null
    & .\phase5out\dxrw.exe shimtrace "phase5\out\geomidx0_$sm.gi.ll" "phase5\out\shimtrace_$sm.cpp.ll" 0,0 | Out-Null
    $same = (Get-FileHash "phase5\out\shimtrace_$sm.py.ll").Hash -eq (Get-FileHash "phase5\out\shimtrace_$sm.cpp.ll").Hash
    $asm = (& .\phase5out\dxilrt.exe asm "phase5\out\shimtrace_$sm.py.ll" "phase5\out\shimtrace_$sm.dxil" 2>&1) -join "`n"
    $valid = $asm -match 'validated and signed ok'
    if ($same -and $valid) { Write-Host "  shimtrace $sm : PASS (byte-identical, validates and signs)" }
    else { Write-Host "  shimtrace $sm : FAIL (identical=$same valid=$valid)"; $failed++ }
    # Arguments computed at run time (0.57.0): each call reads its pair from
    # the shim's table; with 3 structures a switch splits the call's block.
    $d = "phase5\out\shimdyn_$sm.ll"
    & 'C:\DW\DXC\bin\x64\dxc.exe' -T $sm -Fc $d 'phase5\cases\shimtrace_dyn.hlsl' | Out-Null
    foreach ($c in 1, 3) {
        & python phase5\rewriter\shimtrace.py $d "phase5\out\shimdyn_${sm}_$c.py.ll" --copies $c | Out-Null
        & .\phase5out\dxrw.exe shimtrace $d "phase5\out\shimdyn_${sm}_$c.cpp.ll" --copies $c | Out-Null
        $same = (Get-FileHash "phase5\out\shimdyn_${sm}_$c.py.ll").Hash -eq (Get-FileHash "phase5\out\shimdyn_${sm}_$c.cpp.ll").Hash
        $asm = (& .\phase5out\dxilrt.exe asm "phase5\out\shimdyn_${sm}_$c.py.ll" "phase5\out\shimdyn_${sm}_$c.dxil" 2>&1) -join "`n"
        $valid = $asm -match 'validated and signed ok'
        if ($same -and $valid) { Write-Host "  shimtrace $sm run-time arguments, $c structure(s) : PASS (byte-identical, validates and signs)" }
        else { Write-Host "  shimtrace $sm run-time arguments, $c structure(s) : FAIL (identical=$same valid=$valid)"; $failed++ }
        # Several scenes (0.59.0): the first call traces scene slot 1, the
        # second slot 0.
        & python phase5\rewriter\shimtrace.py $d "phase5\out\shimdyn2_${sm}_$c.py.ll" --copies $c --slots 2 1,0 | Out-Null
        & .\phase5out\dxrw.exe shimtrace $d "phase5\out\shimdyn2_${sm}_$c.cpp.ll" --copies $c --slots 2 1,0 | Out-Null
        $same = (Get-FileHash "phase5\out\shimdyn2_${sm}_$c.py.ll").Hash -eq (Get-FileHash "phase5\out\shimdyn2_${sm}_$c.cpp.ll").Hash
        $asm = (& .\phase5out\dxilrt.exe asm "phase5\out\shimdyn2_${sm}_$c.py.ll" "phase5\out\shimdyn2_${sm}_$c.dxil" 2>&1) -join "`n"
        $valid = $asm -match 'validated and signed ok'
        if ($same -and $valid) { Write-Host "  shimtrace $sm two scenes, $c structure(s) : PASS (byte-identical, validates and signs)" }
        else { Write-Host "  shimtrace $sm two scenes, $c structure(s) : FAIL (identical=$same valid=$valid)"; $failed++ }
    }
}

Write-Host ''
Write-Host '=== refusal checks ==='
& python phase5\rewriter\test_reject.py
if ($LASTEXITCODE -ne 0) { $failed++ }

Write-Host ''
if ($failed -eq 0) {
    Write-Host 'REWRITER: all patterns match WARP, all refusals behaved.'
} else {
    Write-Host "REWRITER: $failed check(s) failed."
}
Pop-Location
exit $failed
