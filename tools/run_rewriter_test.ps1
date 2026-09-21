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
       desc = 'commits BOTH kinds: two hit groups from one Proceed loop' }
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
