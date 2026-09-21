# Phase 2: hand-lowering test - notes

Proves the RayQuery -> TraceRay lowering by running the same rays two ways over
one shared acceleration structure and diffing the results.

## What the app does (`phase2/raytest.cpp`)

- Scene: one triangle at z=0, one BLAS, one TLAS (identity instance).
- Rays: a 256x256 orthographic grid looking down -Z from z=2. About 22% hit the
  triangle, the rest miss, so both the closest-hit and miss paths are exercised.
- Per ray it writes `{ float t; float bx; float by; uint hit; }`.
- Pattern: opaque closest-hit only (`RAY_FLAG_FORCE_OPAQUE`, no any-hit shader),
  which is the plan's first target.

Two code paths, identical ray math:
- A, ground truth: `RayQuery<RAY_FLAG_FORCE_OPAQUE>` in a compute shader
  (`cs_6_5`), run on WARP.
- B, lowered: raygen `TraceRay` + closest-hit + miss (`lib_6_3`), run on the
  hardware GPU.

Both bind the same root signature (b0 CBV, t0 = TLAS SRV, u0 = output UAV, all
root descriptors, no descriptor heap). Shaders are compiled at runtime with DXC
and signed by dxil.dll.

## Build / run (Windows x64, Agility SDK)

    build_phase2.bat C:\path\to\agility     (or set AGILITY_SDK_DIR)
    raytest.exe                             both patterns (opaque, alpha):
                                            WARP RayQuery vs HW TraceRay, diffed
    raytest.exe warp rayquery alpha a.bin   run one trial
    raytest.exe hw   traceray alpha b.bin
    raytest.exe diff a.bin b.bin

`D3D12Core.dll` from the Agility SDK must sit in `.\D3D12\` next to the exe (the
batch copies it). `dxcompiler.dll` + `dxil.dll` must be on PATH or beside the
exe. Diff tolerance is 1e-3 on t and barycentrics; hit/miss must match exactly.
Exit 0 = MATCH, 4 = DIVERGE.

## Status

- Opaque closest-hit: PASSED on the dev machine (GTX 1070). WARP reported
  RaytracingTier 0xb (1.1), the 1070 reported 0xa (1.0). Both produced 14450
  hits over 65536 rays and the diff was bit-exact: 0 hit/miss mismatches, 0
  value mismatches, max |dt| = 0, max |dbary| = 0. WARP and NVIDIA returned
  identical t and barycentrics, better than the tolerance the plan allowed for.
- Alpha-tested closest-hit: added (non-opaque geometry, shared alphaTest() as
  the RayQuery Proceed() loop body and as the TraceRay any-hit shader). Expected
  to accept ~8117 of the 14450 triangle hits, so it exercises both accept and
  IgnoreHit(). Awaiting a run on the dev machine.

## Next

- Run the alpha pattern, confirm MATCH.
- With both patterns matching, the Phase 2 gate is passed; proceed to Phase 3
  (proxy d3d12.dll skeleton).
