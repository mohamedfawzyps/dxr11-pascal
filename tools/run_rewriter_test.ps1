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

$cases = @(
    @{ name = 'opaque'; desc = 'pattern 1, opaque closest hit' },
    @{ name = 'alpha';  desc = 'pattern 3, alpha-tested, generated any-hit' }
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

    & python phase5\rewriter\dxrewrite.py lower "phase5\dxil\rayquery_$n.ll" "phase5\out\$n.ll"
    if ($LASTEXITCODE -ne 0) { $failed++; continue }

    $asm = & .\phase5out\dxilrt.exe asm "phase5\out\$n.ll" "phase5\out\$n.dxil" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  ASSEMBLE/VALIDATE FAILED'
        $asm | ForEach-Object { "    $_" }
        $failed++; continue
    }
    Write-Host '  assembled, validated and signed'

    # WARP RayQuery is the oracle; the 1070 runs what the rewriter produced.
    & .\raytest.exe warp rayquery $n "rw_${n}_a.bin" | Out-Null
    & .\raytest.exe hw traceray $n "rw_${n}_b.bin" --lib "phase5\out\$n.dxil" | Out-Null
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
