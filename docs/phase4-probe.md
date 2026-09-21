# Phase 4 probe: the three non-shader Tier 1.1 features

Harness: `phase4/tier11probe.cpp`, built with `build_phase4.bat` into
`phase4out\tier11probe.exe`. Run it with no arguments for both adapters, or
`warp` / `hw` for one, and add `-debug` to turn on the D3D12 debug layer and
drain its InfoQueue after every probe.

This is an instrument, not a pass/fail test. It establishes ground truth on
WARP, which implements Tier 1.1 correctly in software, and records exactly how
each feature fails on the GTX 1070. The shim has to reproduce the first and
intercept the second, so both halves are the deliverable.

It builds into its own directory on purpose. The repo root holds our proxy
`d3d12.dll`, and an exe built there would load the proxy instead of the system
runtime. To run the probe *through* the proxy later, copy `d3d12.dll` into
`phase4out\` and run it again.

## The scene

Same 256x256 ortho grid as Phase 2, so triangle hit counts stay directly
comparable. Two instances in the TLAS:

- instance 0, one triangle at z=0, 14450 hits
- instance 1, one procedural AABB box, x in [-1.45,-1.05], z in [-1.10,-0.90],
  2312 hits

The box sits entirely left of the triangle, so the two never overlap in XY and
the counts stay cleanly separable. Having both geometry types present is what
makes the ray-flag result meaningful. On a triangle-only scene
`SKIP_PROCEDURAL_PRIMITIVES` is a degenerate test that reads as a pass whether
the flag is honoured or ignored.

## Results (2026-09-21)

| | WARP, Tier 1.1 | GTX 1070, Tier 1.0 |
|---|---|---|
| `RaytracingTier` | 0xb | 0xa |
| `HighestShaderModel` | 0x67 | 0x67 |
| `ID3D12Device7` | present | **present** |
| indirect DispatchRays | works, MATCH | `CreateCommandSignature` fails |
| AddToStateObject | works, added shader ran | `CreateStateObject` fails |
| SKIP_TRIANGLES | 0 tri + 2312 proc | **0 tri + 2312 proc** |
| SKIP_PROCEDURAL_PRIMITIVES | 14450 tri + 0 proc | **14450 tri + 0 proc** |

## Finding 1: the new ray flags already work on Pascal, no shim needed

The headline result. Both Tier 1.1 ray flags behave identically on the GTX 1070
and on WARP, to the ray:

    none (baseline)             :  14450 triangle +  2312 procedural
    SKIP_TRIANGLES              :      0 triangle +  2312 procedural
    SKIP_PROCEDURAL_PRIMITIVES  :  14450 triangle +     0 procedural

The shader is `lib_6_5`, since these flags are Shader Model 6.5. It compiles,
the state object is created, the dispatch runs, and the flags are honoured. The
NVIDIA Tier 1.0 driver implements them correctly despite reporting Tier 1.0.

This is verified, not inferred. The dangerous outcome would be a driver that
accepts the shader and silently ignores the flag, which would show as the
baseline count in rows two and three. It does not. Every cell was written in
every run, so no dispatch was quietly skipped.

**Consequence for Phase 4: one of the three features needs no work at all.**
The brief lists ray flags as "map onto existing masks and flags"; on this
hardware even that is unnecessary. Treat it as free and spend the effort on the
other two.

Scope of the claim: verified for `RAY_FLAG_SKIP_TRIANGLES` and
`RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES` used in `TraceRay`, in a `lib_6_5` DXR 1.0
state object, on this driver. Not tested inside `RayQuery`, which is Phase 5's
problem anyway, and not tested on Turing GTX 16-series.

## Finding 2: indirect DispatchRays, one interception point

`CreateCommandSignature` is where it stops, and the debug layer names it
precisely:

    D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS can't be used in a command
    signature unless the device supports D3D12_RAYTRACING_TIER_1_1+.

That is a device method, which `Dxr11Device` already wraps, so the seat exists.
Shapes confirmed on WARP:

- `ByteStride` = `sizeof(D3D12_DISPATCH_RAYS_DESC)` = **104**
- `pRootSignature` is null when the signature carries no root arguments
- one `D3D12_INDIRECT_ARGUMENT_DESC` of type `DISPATCH_RAYS`
- the argument buffer holds a plain `D3D12_DISPATCH_RAYS_DESC`
- `ExecuteIndirect(cs, 1, argBuffer, 0, nullptr, 0)`, after `SetPipelineState1`
  and the root bindings, produces identical results to a direct `DispatchRays`:
  14450 + 2312 both ways

## Finding 3: AddToStateObject, two interception points

It fails earlier than expected. `ID3D12Device7` is present on the 1070, so
`AddToStateObject` is callable, but the Tier 1.0 driver rejects the base state
object before we ever get there:

    ID3D12Device::CreateStateObject: Invalid D3D12_STATE_OBJECT_FLAGS: 0x4

0x4 is `ALLOW_STATE_OBJECT_ADDITIONS`. So the shim needs **two** interception
points, not one:

1. `CreateStateObject`, to strip the flag before forwarding and cache the
   subobjects so the object can be rebuilt later.
2. `ID3D12Device7::AddToStateObject`, to rebuild the full state object from the
   cached subobjects plus the addition.

**`ID3D12Device7` being present on Tier 1.0 hardware confirms the wrapper gap
recorded at the end of Phase 3b.** `Dxr11Device::QueryInterface` currently
passes Device6 and above through unwrapped, so an app would get a real device
and bypass the shim entirely. Extending the wrapper to Device7 is a prerequisite
for this feature, not an optional tidy-up.

### API shape, measured on WARP

Two things about an addition that are not obvious, both found by turning the
debug layer on after an unexplained `E_INVALIDARG`:

- **The configs are not inherited by new exports.** An addition carrying only
  the DXIL library fails with: *"Subobject association of type
  RAYTRACING_SHADER_CONFIG must be defined for all relevant exports, yet no such
  subobject exists at all. An example of an export needing this association is
  Miss2."* and the same for `RAYTRACING_PIPELINE_CONFIG`. An addition must
  repeat both.
- **The addition needs the config flag too**, not just the object being grown:
  *"both the addition and the existing state object must specify
  ALLOW_STATE_OBJECT_ADDITIONS ... In this case the addition is missing the
  flag."*

So a working addition carries: `STATE_OBJECT_CONFIG` with
`ALLOW_STATE_OBJECT_ADDITIONS`, the new `DXIL_LIBRARY`,
`RAYTRACING_SHADER_CONFIG`, and `RAYTRACING_PIPELINE_CONFIG`. The global root
signature and the existing exports *are* inherited. After growth the old
identifiers stay valid and the new export resolves: dispatching with the added
`Miss2` gave 14450 + 2312 + 48774 = 65536 exactly.

That matters directly for the shim, which emulates this by rebuilding from
cached subobjects. It has to carry the configs across for every newly added
export.

## Two harness gotchas worth not rediscovering

- **A BLAS carries one geometry type.** Putting a triangle and an AABB in the
  same bottom-level structure makes the NVIDIA driver return a zero-sized
  prebuild info, which surfaces two calls later as a baffling `E_INVALIDARG`
  from `CreateCommittedResource`. WARP tolerates the mix, so this only appears
  on hardware. `BuildAS` now checks for zero prebuild sizes and says what
  actually happened.
- **Hit group selection is silent when wrong.** With separate BLASes, set
  `InstanceContributionToHitGroupIndex` per instance and leave TraceRay's
  geometry multiplier at 0. Get it wrong and the procedural instance lands on
  the triangle hit group, which has no intersection shader, so the box simply
  never registers a hit and the probe reports a confident zero.

## What Phase 4 should do with this

1. Extend `Dxr11Device` to `ID3D12Device7`. Prerequisite for item 2.
2. `AddToStateObject`: intercept `CreateStateObject` to strip
   `ALLOW_STATE_OBJECT_ADDITIONS` and cache subobjects, then implement
   `AddToStateObject` as a rebuild.
3. Indirect DispatchRays: intercept `CreateCommandSignature`, and handle
   `ExecuteIndirect` by reading the argument buffer back and issuing a direct
   `DispatchRays`. This is the one with real engineering cost, per the brief:
   command list splitting while preserving resource state and
   `ExecuteCommandLists` ordering.
4. Ray flags: nothing to do.
5. Only then flip `CheckFeatureSupport` to report Tier 1.1.
