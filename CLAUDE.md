# dxr11-pascal: DXR Tier 1.1 compatibility shim for NVIDIA Pascal

Version 1.

## Current position (2026-09-21)

- Phase 1 signing test: PASSED. See below and docs/phase1-signing.md.
- Phase 2 hand-lowering test: PASSED, bit-exact on both patterns. See
  docs/phase2-lowering.md.
- Phase 3a forwarding-only proxy: PASSED. Phase 3b ID3D12Device5 wrapper:
  PASSED. See docs/phase3-proxy.md.
  Next action: Phase 4, the three non-shader features.

  Two findings worth carrying forward, both about the proxy's export table:
  - A proxy d3d12.dll must match the real DLL's export ORDINALS, not just its
    names. The Windows SDK's d3d12.lib imports D3D12CreateDevice by ordinal
    101, so a name-only proxy dies at load with STATUS_ORDINAL_NOT_FOUND
    (0xC0000138) before any of our code runs.
  - The proxy MUST also export D3D12CoreCreateLayeredDevice,
    D3D12CoreGetLayeredDeviceSize and D3D12CoreRegisterLayers, which
    d3d12SDKLayers.dll imports from d3d12.dll by name. Without them the D3D12
    debug layer silently does not run, reported as the misleading
    "SDKLayers dll not found at D3D12SDKPath" (0x887E0003). Their signatures
    are undocumented, so proxy/d3d12_thunks.asm forwards them as x64 tail
    jumps, which need no signature. Keep the debug layer working: it is the
    tool that catches our own mistakes in phases 4 and 5.

  The device wrapper is `Dxr11Device` in proxy/d3d12_device.{h,cpp}: all 62
  ID3D12Device5 methods forwarded unchanged. It wraps only the device, not
  child objects, so ID3D12DeviceChild::GetDevice() still returns the real
  device; Phase 4 has to handle that. QueryInterface for anything above
  Device5 is passed through unwrapped AND logged, so watch the log for
  ID3D12Device7 (AddToStateObject) before relying on Phase 4 interception.
  DXR11_NO_WRAP=1 disables wrapping, restoring forward-only behaviour.

Dev machine (Windows x64), everything under `C:\DW`:
- `C:\DW\dxr11-pascal` - this repository.
- `C:\DW\DXC` - DirectX Shader Compiler (`inc\dxcapi.h`, `bin\x64\dxcompiler.dll`,
  `dxil.dll`).
- `C:\DW\microsoft.direct3d.d3d12.1.619.5` - Agility SDK.
- `C:\DW\DirectX-Graphics-Samples` - Microsoft samples, source of the DXR 1.0
  app used to validate the proxy.
- Build scripts: `build.bat` (phase 1), `build_phase2.bat`, `build_proxy.bat`,
  `build_sample.bat`.
  `build_sample.bat` builds a Microsoft DXR 1.0 sample used to validate the
  proxy, with cl.exe and no NuGet restore, into `sampletest\<SampleName>\`.
  Helpers live in `tools\`.
  Each calls `setup_msvc.bat`, which finds MSVC via vswhere and activates the
  x64 toolchain, so they work from any terminal. Running them from an
  "x64 Native Tools Command Prompt for VS" also still works, the helper
  detects cl.exe and does nothing.

Optional read-only reference: a local clone of the Unreal Engine 5.7 NvRTX
Caustics branch. Useful from Phase 4 on, to see how a shipping engine queries
the raytracing tier and drives both dispatch paths. Look in
`Engine/Source/Runtime/D3D12RHI` and grep narrowly, the repo is enormous. It is
a separate project (see Related); read it for reference, do not vendor it here
or let it redirect this project's scope.

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

**Result: PASSED (2026-09-21).** `src/signtest.cpp` (build with `build.bat`)
runs two trials through `dxil.dll`'s `IDxcValidator`:
- Valid DXIL with a corrupted header digest re-signs cleanly, and the
  recomputed digest exactly reproduces the original, so the digest is a
  deterministic hash with no secret key.
- A flipped DXIL bytecode byte is rejected ("Malformed block"), so the
  validator signs only DXIL that still validates.

The rewriting path is open: emit valid DXIL, let `dxil.dll` sign it. No
substitution fallback needed. Detail in docs/phase1-signing.md.

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

**Result: PASSED (2026-09-21).** harness in `phase2/raytest.cpp`, details in
docs/phase2-lowering.md. WARP (Tier 1.1) vs GTX 1070 (Tier 1.0), 65536 rays:
- Opaque closest-hit: 14450 hits, bit-exact diff.
- Alpha-tested closest-hit (any-hit / Proceed loop-body lowering): 8117 hits,
  bit-exact diff.

Both t and barycentrics were identical between WARP and NVIDIA, better than the
tolerance allowed. The lowering is sound for these patterns; proceed to Phase 3.

### Phase 3: proxy skeleton (1 week)

Proxy `d3d12.dll`. Wrap `ID3D12Device5` and friends, forward everything
unchanged, verify a real DXR 1.0 app still runs through it. No translation yet.

**Result: 3a PASSED (2026-09-21).** Forwarding-only proxy, no device wrapping.
Detail in docs/phase3-proxy.md. Validated on two DXR 1.0 apps on the GTX 1070:
- `raytest.exe` (the Phase 2 harness) still reports ALL MATCH through the proxy,
  and the log catches all four device creations.
- `D3D12RaytracingHelloWorld` (Microsoft sample, windowed, swapchain, Agility
  SDK) renders **pixel-identical** to the no-proxy baseline, 0 differing pixels,
  same ~2140 fps.
- `D3D12RaytracingSimpleLighting`, which exercises more device surface per frame
  (3-descriptor heap, per-frame CB, index/vertex buffers with normals), runs
  identically through the proxy at the same ~1380 fps. No pixel claim is made
  for it: our window capture is not a reliable oracle for an animated
  flip-model swapchain, see the caveat in docs/phase3-proxy.md.

`build_sample.bat <SampleName> [debug]` builds either sample;
`tools
un_proxy_test.ps1 -Exe ... [-Animated]` runs one with and without the
proxy and reports the diff. Both samples carry the same upstream
uninitialised-`m_descriptorsAllocated` bug, patched at build time by
`tools\patch_sample.ps1`.

**Result: 3b PASSED (2026-09-21).** `Dxr11Device` wraps the device and forwards
all 62 methods. Re-verified with the same three apps: raytest ALL MATCH,
HelloWorld 0 of 14400 pixels differ, SimpleLighting unchanged fps. The D3D12
debug layer runs clean through the wrapper, with the same single pre-existing
warning as baseline and no errors.

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
