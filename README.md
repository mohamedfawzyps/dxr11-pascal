# dxr11-pascal

A user-mode D3D12 layer that makes GPUs reporting `D3D12_RAYTRACING_TIER_1_0`
present as Tier 1.1, by translating the Tier 1.1 features into DXR 1.0
operations the driver already supports.

Target hardware is NVIDIA Pascal (GTX 10-series) and Turing GTX 16-series.
Both report Tier 1.0. Development and every measurement in this repository are
on a GTX 1070.

**A RayQuery compute shader, unmodified, runs on a GTX 1070 and produces
bit-exact output against WARP.**

## The premise

NVIDIA's driver has supported DXR 1.0 on Pascal since driver 425.31 (April
2019), running BVH traversal and ray/triangle intersection on the shader cores
instead of RT cores. **The GPU already traces rays.** What Pascal lacks is the
Tier 1.1 API surface, not the ray tracing capability.

So this project never implements ray tracing. It translates API shapes and lets
the driver do all the ray work. That is what makes it correct rather than
approximate: rays hit NVIDIA's own acceleration structures, traversed by
NVIDIA's own code, so results match an RTX card by construction.

There is no custom BVH here and no software traversal. Both were considered and
rejected.

## What it does

| Tier 1.1 feature | How |
|---|---|
| `RAY_FLAG_SKIP_TRIANGLES` / `SKIP_PROCEDURAL_PRIMITIVES` | Nothing. Measured against WARP: the Tier 1.0 driver already honours them |
| `AddToStateObject` | `CreateStateObject` caches the subobjects, additions rebuild a whole new state object |
| Indirect `DispatchRays` | The command list is split at the dispatch, the argument buffer is read back, real `DispatchRays` calls are issued |
| Inline ray tracing (`RayQuery`) | The DXIL is rewritten into a DXR 1.0 library, with generated any-hit, intersection, closest-hit and miss shaders |

The fourth is the real work. A `RayQuery` compute shader is lowered so that the
`Proceed()` loop body becomes an any-hit shader, or an intersection shader, or
both, and the committed accessors travel in the ray payload.

## Status

Verified on a GTX 1070, with WARP as the oracle for every result:

- 14 end-to-end render cases, all bit-exact, plus 2 refusal gates
- 12 rewriter cases, each byte-identical between the Python reference and the
  C++ port, on both the `.ll` path and the DXIL container path
- Two Microsoft DXR 1.0 samples run through the proxy unchanged, one of them
  pixel-identical to its no-proxy baseline

The tier flip is **opt-in**, behind `DXR11_TIER11=1`. Without it the shim
reports Tier 1.0 and forwards everything. Reporting 1.1 entitles an application
to emit `RayQuery`, and a shim that claims 1.1 and then fails is worse than one
that claims 1.0.

## Using it

Build the proxy, then put the resulting `d3d12.dll` next to the target
executable:

```
build_proxy.bat
copy d3d12.dll <target directory>
set DXR11_TIER11=1
```

`DXR11_NO_WRAP=1` disables device wrapping and restores plain forwarding, which
is useful for isolating whether a problem is the shim at all. The log lands in
`%TEMP%\dxr11_proxy.log` and says what was intercepted, what was lowered and
what was refused, with reasons.

## What it refuses, and why

A shader the rewriter cannot lower is logged and forwarded unchanged, so the
application gets the driver's own error rather than a silently wrong render.
Nothing here is guessed at: each refusal is provoked by a test.

Refused because DXR 1.0 offers nothing to lower onto:

- `CommittedGeometryIndex` and `CandidateGeometryIndex`. Measured:
  `GeometryIndex()` inside a DXR 1.0 hit shader is *itself* Tier 1.1, and the
  driver rejects the state object
- `*InstanceContributionToHitGroupIndex`, which HLSL does not expose to a hit
  shader at all
- `RayQuery` in a pixel, vertex, mesh or amplification shader
- `RayQuery` inside an existing DXR 1.0 shader
- `groupshared` memory, group barriers or wave intrinsics in the same entry
  point as the query
- more than one `RayQuery` object per entry point
- a loop body reading caller locals that do not fit the payload
- an application routing both triangle and procedural geometry to the same hit
  group record, which no shader table can serve

Unreal Engine 5.7 uses 25 distinct `RayQuery` accessors. Everything with a DXR
1.0 equivalent is supported, which is 21 of them; the other four are in the
list above.

## Requirements

- Windows x64, a GPU reporting `D3D12_RAYTRACING_TIER_1_0`
- MSVC. The build scripts find it through `vswhere` and work from any terminal
- [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)
  and the [D3D12 Agility SDK](https://devblogs.microsoft.com/directx/directx12agility/),
  supplied by you. Nothing from NVIDIA or Microsoft is redistributed here

Paths are set at the top of the build scripts.

## Testing

WARP is the oracle throughout. It implements Tier 1.1 correctly in software, so
any `RayQuery` shader can be run there for ground truth and diffed against the
lowered version on real hardware. No RTX card is needed to develop this.

```
tools\run_dispatch_test.ps1     RayQuery shaders end to end through the proxy
tools\run_rewriter_test.ps1     the rewriter, and the Python/C++ agreement
```

Ground truth is taken from WARP on every run rather than from a stored
baseline, and each sensitivity check is measured rather than assumed. Several
findings in `docs/` are records of tests that passed for the wrong reason and
what it took to notice.

## Documentation

`docs/` carries the reasoning and the measurements behind each phase, including
the designs that turned out to be wrong. `CLAUDE.md` is the working brief.

## Disclaimer

Not affiliated with, endorsed by or supported by NVIDIA or Microsoft. This
project redistributes no code belonging to either. It is a compatibility layer
built against public APIs.
