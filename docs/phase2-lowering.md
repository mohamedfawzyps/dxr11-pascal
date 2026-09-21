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
    raytest.exe                             WARP RayQuery -> a.bin,
                                            HW TraceRay  -> b.bin, then diff
    raytest.exe warp rayquery a.bin         run one trial
    raytest.exe hw   traceray b.bin
    raytest.exe diff a.bin b.bin

`D3D12Core.dll` from the Agility SDK must sit in `.\D3D12\` next to the exe (the
batch copies it). `dxcompiler.dll` + `dxil.dll` must be on PATH or beside the
exe. Diff tolerance is 1e-3 on t and barycentrics; hit/miss must match exactly.
Exit 0 = MATCH, 4 = DIVERGE.

## Status

- Host-side geometry and diff logic verified on Linux (hit/miss mix correct,
  hits at t==camZ, diff flags value and hit-flip divergence).
- The D3D12 / DXR plumbing was written without a Windows build to hand, so the
  first compile on the dev machine is an iteration pass. Verified-vs-inferred:
  the ray math and diff are verified; the D3D12 API calls are inferred and need
  a real build+run.

## Next

- Run it, confirm MATCH on the opaque closest-hit pattern.
- Then add the alpha-tested pattern (generated any-hit shader / `IgnoreHit`) to
  exercise the `Proceed()` loop-body lowering, per the plan.
