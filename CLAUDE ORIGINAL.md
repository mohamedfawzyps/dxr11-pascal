# dxr11-pascal: DXR Tier 1.1 compatibility shim for NVIDIA Pascal

Version 1.

## What this is

A user-mode D3D12 layer (proxy `d3d12.dll`) that makes GPUs reporting
`D3D12_RAYTRACING_TIER_1_0` present as Tier 1.1, by translating the four Tier
1.1 features into DXR 1.0 operations the driver already supports.

Target hardware: NVIDIA Pascal (GTX 10-series, dev machine has a GTX 1070) and
Turing GTX 16-series. Both report Tier 1.0.

No application or engine modification. Performance is not a concern; 1 fps is
an acceptable result. Correctness is what matters.

## The premise this rests on

NVIDIA's driver has supported DXR 1.0 on Pascal since driver 425.31 (April
2019), running BVH traversal and ray/triangle intersection on the shader cores
instead of RT cores. **The GPU already traces rays.** What Pascal lacks is the
Tier 1.1 API surface, not the ray tracing capability.

So the shim never implements ray tracing. It translates API shapes and lets
the driver do all ray work. This matters for correctness: rays hit NVIDIA's
own acceleration structure, traversed by NVIDIA's own code, so results match
an RTX card by construction.

**Do not build a custom BVH. Do not write software traversal.** Both were
considered and rejected: they would be slower than the driver's path, would
diverge from real DXR results, and would require parsing or replacing an
acceleration structure the driver already builds correctly.

## The four Tier 1.1 features

| Feature | Approach | Shader work? |
|---|---|---|
| Indirect DispatchRays (`ExecuteIndirect` + `DISPATCH_RAYS`) | CPU readback of the argument buffer, then direct `DispatchRays` | No |
| `AddToStateObject` (`ID3D12Device7`) | Rebuild the full state object from cached subobjects | No |
| `RAY_FLAG_SKIP_TRIANGLES` / `SKIP_PROCEDURAL_PRIMITIVES` | Map onto existing masks and flags | No |
| Inline ray tracing (`RayQuery` / `TraceRayInline`) | Lower to `TraceRay` + generated hit/miss shaders | **Yes** |

The first three are the cheap half and deliver a useful product on their own.
The fourth is the real project.

## The RayQuery to TraceRay mapping

| RayQuery construct | DXR 1.0 equivalent |
|---|---|
| `RayQuery<FLAGS> q` | No object; FLAGS become the `TraceRay` RayFlags argument |
| `q.TraceRayInline(AS, flags, mask, ray)` | `TraceRay(AS, FLAGS\|flags, mask, 0, 0, 0, ray, payload)` |
| `while (q.Proceed())` loop body | Generated **any-hit shader** |
| `q.CommitNonOpaqueTriangleHit()` | Any-hit returns normally (accept) |
| not committing a candidate | Any-hit calls `IgnoreHit()` |
| `q.CommitProceduralPrimitiveHit(t)` | Generated **intersection shader** calling `ReportHit(t, ...)` |
| `q.Abort()` | `AcceptHitAndEndSearch()`, or a payload flag |
| `q.CommittedStatus()` | Which terminal shader ran: closest-hit vs miss |
| `Committed*` accessors | Payload fields, filled by the generated closest-hit shader from `RayTCurrent()`, `InstanceIndex()`, `PrimitiveIndex()`, `GeometryIndex()`, barycentric attributes, `HitKind()` |

The key correspondence: **the any-hit shader is `Proceed()`'s loop body.**
Accept equals fall off the end, reject equals `IgnoreHit()`.

`MaxTraceRecursionDepth` can be 1, since inline queries do not recurse.

## Start with the easy patterns

Do not attempt general RayQuery first. These three cover most real usage and
are much simpler:

1. **Opaque closest hit.** With `RAY_FLAG_FORCE_OPAQUE`, `Proceed()` never
   yields a candidate and traversal is entirely fixed-function. Needs a
   closest-hit and a miss shader, and **no any-hit shader at all**. This is
   the first target.
2. **Shadow / visibility.** `ACCEPT_FIRST_HIT_AND_END_SEARCH`, miss shader
   only.
3. **Alpha-tested closest hit.** Adds the generated any-hit shader.

## Cases with no valid lowering

Detect these and fail loudly rather than producing wrong output:

| Pattern | Why |
|---|---|
| RayQuery in a pixel, vertex, mesh or amplification shader | `DispatchRays` only launches raygen; there is no promotion path |
| RayQuery inside an existing DXR 1.0 shader | Any-hit and intersection shaders cannot call `TraceRay` |
| RayQuery alongside `groupshared` or group barriers | Raygen shaders have no thread group |
| Multiple concurrent RayQuery objects | `TraceRay` has one payload and one in-flight trace |
| Loop body reading caller locals that do not fit the payload | The any-hit shader is a separate invocation; the payload is the only shared state |
| Wave intrinsics around the query | Promotion to raygen changes lane occupancy |

The pixel-shader case is the most consequential, since it is legal in DXR 1.1
and some engines use it.

## Build order

Do not start with the proxy. The first two phases are experiments that decide
whether the rest is worth building.

### Phase 1: signing test (1 day) - GATE

Compile a shader with stock DXC. Modify something harmless in the DXIL
container. Load `dxil.dll`, get `IDxcValidator` via `DxcCreateInstance`
(`CLSID_DxcValidator`), and try to validate and re-sign it. Graham Wihlidal
documented this workflow with `DxcValidatorFlags_InPlaceEdit`.

- **Signs:** the DXIL rewriting path is open. Proceed.
- **Refuses:** switch to shader substitution (pre-compile replacements from
  HLSL with stock DXC, swap by hash at `CreateStateObject`). Coverage
  narrows to shaders you have source for.

Note: `D3D12EnableExperimentalFeatures(D3D12ExperimentalShaderModels)` is NOT
the answer here. It unlocks unfinalized shader models, not arbitrary unsigned
DXIL, and Agility SDK 1.618/1.619 made it return `E_NOINTERFACE`. RayQuery is
SM 6.5, already finalized, so nothing experimental is needed.

### Phase 2: hand-lowering test (2-3 days) - GATE

Minimal D3D12 app, no shim, no Unreal. One triangle scene, one TLAS, one BLAS.
Write two versions of the same query by hand:

- **A:** compute shader using `RayQuery`, run on **WARP** (which supports
  Tier 1.1, select via `IDXGIFactory4::EnumWarpAdapter`). This is ground truth.
- **B:** raygen shader using `TraceRay` with generated payload and hit
  shaders, run on the **GTX 1070**.

Diff the output buffers. Start with the opaque closest-hit pattern, then add
alpha testing to exercise the any-hit path.

- **Match:** the lowering is sound. Proceed.
- **Diverge:** find out why by hand now, not inside a compiler later.

Use a tolerance, not exact equality. DXR does not guarantee bit-identical
results across implementations.

### Phase 3: proxy skeleton (1 week)

Proxy `d3d12.dll`. Wrap `ID3D12Device5` and friends, forward everything
unchanged, verify a real DXR 1.0 app still runs through it. No translation yet.

### Phase 4: the three non-shader features (1-2 weeks)

Indirect DispatchRays, `AddToStateObject`, the new ray flags. Report Tier 1.1
only once these work.

**Warning:** reporting Tier 1.1 entitles the app to emit RayQuery. Until
phase 5 works, either keep reporting 1.0, or detect RayQuery DXIL and fail
clearly. A shim that claims 1.1 and then crashes is worse than one that
claims 1.0.

Main engineering cost here is command list splitting for the indirect case:
preserving resource state and `ExecuteCommandLists` ordering across the break.

### Phase 5: the DXIL rewriter (months)

Automate phase 2's transform. DXIL is LLVM 3.7 bitcode. Parse, detect
`dx.op.rayQuery_*` opcodes, generate the raygen/any-hit/closest-hit/miss set,
synthesize the state object and shader table, re-sign.

Reference implementations to read before writing anything:
- **dxil-spirv** (HansKristian-Work): a DXIL parser with its own lightweight
  LLVM bitcode reader, already handles ray tracing opcodes. Its front end is
  the reusable half; it emits SPIR-V, not DXIL, so the back end is not.
- **Maister's "My personal hell of translating DXIL to SPIR-V"**, parts 1-4.
- **Mesa RADV** `radv_nir_lower_ray_queries.c`: the equivalent lowering at NIR
  level, for the algorithm.
- **Microsoft's archived D3D12 Raytracing Fallback Layer**: architecture and
  its documented limitations. Note it never supported inline ray tracing.
- **DXIL specs** for the `dx.op.rayQuery_*` opcode list (AllocateRayQuery is
  178; verify current numbers against `DxilConstants.h` at your target DXIL
  version, Microsoft appends opcodes over shader model revisions).

## Testing

WARP is the oracle throughout. It implements Tier 1.1 correctly in software,
so any RayQuery shader can be run there for ground truth and diffed against
the lowered version on the 1070. No RTX card needed.

## Working method

Read the source before designing. In the investigation that produced this
brief, every architecture reasoned out in the abstract turned out to be wrong,
and every correct finding came from reading actual files. Prefer a grep over a
hypothesis.

State clearly what is verified versus inferred. Flag uncertainty rather than
guessing.

## Preferences

- No em dashes. Use commas or other punctuation.
- Keep responses short and easy to follow.
- Version deliverable files rather than overwriting; keep old versions in an
  `archive/` folder.

## Related

There is a separate, smaller project targeting the same goal from inside
Unreal Engine (NvRTX 5.7 Caustics), where Epic already wrote both dispatch
paths and the fix is a handful of edits. That project is independent of this
one and is the faster route if Unreal is the only target.
