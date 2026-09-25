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
# Recursion depth 2: the closest-hits and the miss trace too (0.56.0), with
# the scene in the global root signature, or in every record's local one.
foreach ($b in @('', '--collections', '--grow')) {
    foreach ($x in @('', '--gpuinst', '--indirect', '--indirectgpu', '--stale', '--table')) {
        foreach ($s in @('', '--sm66')) { $cfgs += ,@(@($b, '--recurse', $x, $s) | Where-Object { $_ }) }
    }
    # WARP removes its device when a closest-hit or miss traces a scene from
    # the heap, so its ground truth binds the scene globally (--warpglobal).
    foreach ($x in @('--bindless', '--bindlessrc')) { $cfgs += ,@(@($b, '--recurse', $x, '--warpglobal') | Where-Object { $_ }) }
    foreach ($l in @('--localscene', '--localscenetable')) {
        foreach ($x in @('', '--gpusbt', '--indirectgpu', '--gpuinst', '--stale')) {
            foreach ($s in @('', '--sm66')) { $cfgs += ,@(@($b, '--recurse', $l, $x, $s) | Where-Object { $_ }) }
        }
    }
}
$cfgs += ,@('--recurse', '--localscene', '--indirectgpu', '--gpusbt')
$cfgs += ,@('--recurse', '--localscenetable', '--stale', '--gpusbt', '--sm66')
# The substitute ground truth agrees with WARP's own where WARP works.
$cfgs += ,@('--bindless', '--warpglobal')
# TraceRay arguments computed at run time (0.57.0): from the constants, read
# at the dispatch; with --norefine not read, so every pair counts and the
# shim's layout takes several scene structures; --perstruct 4 makes a few
# pairs take several too.
foreach ($b in @('', '--collections', '--grow')) {
    foreach ($x in @('', '--gpuinst', '--indirect', '--indirectgpu', '--stale', '--table', '--recurse')) {
        foreach ($s in @('', '--sm66')) { $cfgs += ,@(@($b, '--dynargs', $x, $s) | Where-Object { $_ }) }
    }
    foreach ($x in @('--bindless', '--bindlessrc')) { $cfgs += ,@(@($b, '--dynargs', $x) | Where-Object { $_ }) }
    foreach ($x in @('', '--gpuinst', '--recurse')) {
        $cfgs += ,@(@($b, '--dynargs', '--norefine', $x) | Where-Object { $_ })
        $cfgs += ,@(@($b, '--dynargs', '--norefine', '--perstruct', '4', $x, '--sm66') | Where-Object { $_ })
    }
}
$cfgs += ,@('--dynargs', '--libassoc')
$cfgs += ,@('--dynargs', '--localscene', '--gpusbt')
$cfgs += ,@('--dynargs', '--recurse', '--localscene')
$cfgs += ,@('--dynargs', '--recurse', '--bindless', '--warpglobal')
$cfgs += ,@('--dynargs', '--perstruct', '4', '--norefine', '--indirectgpu')
# A library's own subobjects (0.58.0): signatures and associations in the
# library, included through an export list by collections; hit groups there
# too; signatures in a second library; associated by the state object by
# name, explicitly or as its default.
foreach ($b in @('', '--collections', '--grow')) {
    foreach ($m in @('--libassoc', '--libhg', '--libsplit', '--dxilassoc', '--dxildefault')) {
        if (-not $b -and $m -eq '--libassoc') { continue }   # above
        foreach ($s in @('', '--sm66')) { $cfgs += ,@(@($b, $m, $s) | Where-Object { $_ }) }
    }
}
$cfgs += ,@('--libhg', '--recurse')
$cfgs += ,@('--collections', '--libhg', '--recurse')
$cfgs += ,@('--dxildefault', '--recurse')
$cfgs += ,@('--libsplit', '--indirectgpu')
$cfgs += ,@('--dxilassoc', '--stale')
$cfgs += ,@('--dxildefault', '--gpuinst')
$cfgs += ,@('--libhg', '--dynargs')
$cfgs += ,@('--collections', '--dxildefault', '--dynargs')
# Several scenes (0.59.0): the raygen traces two, the closest-hits the
# second, or each hit record carries its own; --twoconflict makes the two
# disagree about a record, so no table labelled for both can serve them.
foreach ($b in @('', '--collections', '--grow')) {
    foreach ($m in @('--twoscenes', '--twoconflict')) {
        foreach ($x in @('', '--recurse', '--sm66')) { $cfgs += ,@(@($b, $m, $x) | Where-Object { $_ }) }
        $cfgs += ,@(@($b, $m, '--localscene', '--recurse') | Where-Object { $_ })
    }
}
$cfgs += ,@('--twoscenes', '--gpuinst')
$cfgs += ,@('--twoconflict', '--stale')
$cfgs += ,@('--twoscenes', '--indirectgpu')
$cfgs += ,@('--twoconflict', '--localscene', '--recurse', '--gpusbt')
$cfgs += ,@('--twoscenes', '--dynargs')
$cfgs += ,@('--twoconflict', '--dynargs', '--norefine', '--perstruct', '8')
$cfgs += ,@('--twoscenes', '--libhg')
# A scene picked per ray (0.60.0): an array at a dynamic element, bounded or
# not, in the global root signature or the records' (where every other hit
# record's table starts one later); the heap at an index computed in the
# shader, or read from a cbuffer in GPU memory.
foreach ($b in @('', '--collections', '--grow')) {
    foreach ($m in @('--scenearray', '--sceneunbounded')) {
        foreach ($x in @('', '--twoconflict', '--recurse', '--sm66')) { $cfgs += ,@(@($b, $m, $x) | Where-Object { $_ }) }
        $cfgs += ,@(@($b, '--localscenetable', $m) | Where-Object { $_ })
        $cfgs += ,@(@($b, '--localscenetable', $m, '--recurse', '--twoconflict') | Where-Object { $_ })
    }
    foreach ($m in @('--heapdyn', '--heapgpu')) { $cfgs += ,@(@($b, $m) | Where-Object { $_ }) }
}
$cfgs += ,@('--scenearray', '--twoconflict', '--gpuinst')
$cfgs += ,@('--scenearray', '--twoconflict', '--stale')
$cfgs += ,@('--scenearray', '--twoconflict', '--indirectgpu')
$cfgs += ,@('--localscenetable', '--scenearray', '--twoconflict', '--gpusbt')
$cfgs += ,@('--scenearray', '--twoconflict', '--dynargs')
$cfgs += ,@('--scenearray', '--twoconflict', '--dynargs', '--norefine', '--perstruct', '8')
$cfgs += ,@('--heapdyn', '--dynargs')
$cfgs += ,@('--heapgpu', '--indirectgpu')
$cfgs += ,@('--heapdyn', '--stale')
$cfgs += ,@('--scenearray', '--libhg')

# A failure keeps its output and the shim's log, so an intermittent one can
# be read afterwards rather than rerun in hope.
$keep = Join-Path $env:TEMP 'gitest-matrix'
New-Item -ItemType Directory -Force $keep | Out-Null
$prevLog = $env:DXR_TIER11_LOG
$fail = @()
$unstable = @()
$n = 0
foreach ($c in $cfgs) {
    $n++
    $env:DXR_TIER11_LOG = Join-Path $keep "run_$n.log"
    $out = & .\gitest.exe $c 2>&1
    if ($LASTEXITCODE -ne 0) {
        $fail += ('[' + ($c -join ' ') + ']')
        $out | Out-File (Join-Path $keep "run_$n.out") -Encoding utf8
        $out | Select-String 'DIVERGE|FAILED' | ForEach-Object { Write-Host "    run ${n}: $($_.Line)" }
    } elseif (($out -join "`n") -match 'GROUND TRUTH UNSTABLE') {
        # WARP differed from itself and the hardware matched its second run:
        # not the shim, but kept and named.
        $unstable += ('[' + ($c -join ' ') + ']')
        $out | Out-File (Join-Path $keep "run_$n.out") -Encoding utf8
    } else {
        Remove-Item $env:DXR_TIER11_LOG -ErrorAction SilentlyContinue
    }
}
# Gate (0.59.0): a variant whose local root signature would pass the GTX
# 1070's driver limit (two scenes of 64 structures, 129 root descriptors) is
# refused by name, and the device lives on: before the check it was removed.
$env:DXR_TIER11_LOG = Join-Path $keep 'gate_rootsig.log'
Remove-Item $env:DXR_TIER11_LOG -ErrorAction SilentlyContinue
$out = & .\gitest.exe --twoconflict --dynargs --norefine --perstruct 4 zero mult 2>&1
$refused = Select-String -Path $env:DXR_TIER11_LOG -Pattern 'measured to survive' -Quiet
$alive = -not (($out -join "`n") -match 'hardware failed')
if ($refused -and $alive) { Write-Host "gate: an oversized local root signature is refused by name, the device lives" }
else { Write-Host "gate FAILED: refused=$refused device alive=$alive"; $fail += '[gate: oversized local root signature]' }
$env:DXR_TIER11_LOG = $prevLog
Write-Host "gitest: $($cfgs.Count) configurations, $($cfgs.Count - $fail.Count) match WARP"
foreach ($f in $fail) { Write-Host "  FAILED $f" }
if ($unstable.Count) {
    Write-Host "  ground truth unstable in $($unstable.Count) (WARP differed from itself, the hardware matched its second run):"
    foreach ($u in $unstable) { Write-Host "    $u" }
}
if ($fail.Count) { Write-Host "  output and shim logs of the failures: $keep" }
Pop-Location
exit $fail.Count
