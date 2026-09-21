# dxr11-pascal: DXR Tier 1.1 compatibility shim for NVIDIA Pascal

Version 1.

## Current position (2026-09-21)

- Phase 1 signing test: PASSED. See below and docs/phase1-signing.md.
- Phase 2 hand-lowering test: PASSED, bit-exact on both patterns. See
  docs/phase2-lowering.md.
- Phase 3a forwarding-only proxy: PASSED. Phase 3b ID3D12Device5 wrapper:
  PASSED. See docs/phase3-proxy.md.
- Phase 4 probe: DONE, ground truth captured. See docs/phase4-probe.md.
  Headline: the Tier 1.1 ray flags ALREADY WORK on the GTX 1070, verified
  against WARP to the ray, so that feature needs no shim at all. The other
  two needed work and are now done, see below. Flip CheckFeatureSupport to
  Tier 1.1 last.
- Device7 wrapper extension: DONE. Dxr11Device now implements ID3D12Device7,
  so AddToStateObject and CreateProtectedResourceSession1 route through the
  shim. Verified by running the Phase 4 probe through the proxy: the
  AddToStateObject call lands in our wrapper on WARP, and no interface is
  passed through unwrapped any more. Regression clean on all three apps.
- AddToStateObject emulation: WORKING on the GTX 1070. See
  docs/phase4-addtostateobject.md. CreateStateObject strips
  ALLOW_STATE_OBJECT_ADDITIONS and deep-copies the subobjects into a store
  attached to the state object via SetPrivateDataInterface;
  AddToStateObject merges that store with the addition and rebuilds a whole
  new state object. On Tier 1.1 both forward untouched, so the shim stays
  transparent where it is not needed. The Phase 4 probe on hardware now
  matches the WARP ground truth exactly, 14450 + 2312 + 48774 = 65536.
- Indirect DispatchRays: WORKING on the GTX 1070. See
  docs/phase4-indirect-design.md. This was the last of the three non-shader
  features, so **all of Phase 4 now works**.

  How it fits together. `CreateCommandSignature` on a DISPATCH_RAYS signature
  returns `Dxr11CommandSignature`, a stand-in that is deliberately not a real
  signature. `ExecuteIndirect` on one records the barriers and a readback copy
  of the argument buffer, then queues a dispatch instead of recording it. The
  recording is thereby cut into segments. At submit time the queue hook
  submits a segment, syncs once, reads the dimensions the GPU produced, and
  issues real `DispatchRays` calls on pooled command lists before submitting
  the continuation.

  Because the split lands in the middle of a recording, binding state has to
  survive it. Each queued dispatch carries its own snapshot of the compute
  root bindings, and the continuation replays the graphics state too, since
  `SetPipelineState1` clobbers the graphics PSO. Still NOT tracked: stream
  output targets, predication, sample positions, shading rate. Nothing tested
  so far sets one across a split.

  Adjacent dispatches share one sync: a split only queues, and the segment
  stays open until the application records work that must be ordered after it.
  Worth 15 to 28 percent on eight dispatches.

  Note that Phase 4 is NOT finished in the sense the build order means it,
  because `CheckFeatureSupport` still reports Tier 1.0 on purpose. Flipping it
  entitles an application to emit RayQuery, so it waits for Phase 5 or for a
  RayQuery detector that fails clearly.

  Three architecture findings, each of which cost a wrong design first:
  - **Wrap the command list, hook the queue.** Not symmetric, and both halves
    were established by measurement rather than argument.
  - Wrapping the command QUEUE breaks DXGI. Creation succeeds and looks fine;
    `Present` then access-violates (0xC000041D) and both Microsoft samples die.
    Testing only `CreateSwapChainForHwnd` will not catch this.
  - Hooking a command LIST's vtable does not work. Command lists get per-object
    heap vtables, so patching the image vtable passes a self-test and then
    never intercepts a real call. Queues do share an image vtable, which is why
    the queue half works; the hook patches slot 10 and self-tests on install.

- Phase 5 recon: DONE, and it changes the plan. See
  docs/phase5-dxil-recon.md. **DXIL survives a round trip through `.ll` text**,
  so the rewriter is a text transformation, NOT LLVM 3.7 bitcode surgery. That
  removes the largest cost in the original estimate and the need for a bitcode
  writer at all. Measured, then proven on hardware: `raytest.exe --roundtrip`
  sends every shader out to text and back before D3D12 sees it and still
  reports ALL MATCH, so a round-tripped `lib_6_3` builds a working state object
  on the 1070 and renders bit-exactly.
- Phase 5 patterns 1 and 3: **BOTH WORKING on the GTX 1070, lowered by hand.**
  `phase5/hand/make_lib.py` edits `rayquery_opaque.ll`, a `cs_6_5` compute
  shader using `RayQuery<RAY_FLAG_FORCE_OPAQUE>`, into a `lib_6_5` DXR library
  purely as text. Against WARP's native Tier 1.1 RayQuery as ground truth:
  14450 of 65536 rays hit on both, 0 mismatches, max |dt| 0.000000. So a
  RayQuery shader transformed as text runs correctly on Tier 1.0 hardware.
  That is the Phase 5 premise demonstrated end to end, for one pattern.

  The work was specified by the validator, not guessed. The first attempt
  changed exactly one thing, `cs` to `lib`, and asked; it named both root
  causes, and the recon had predicted both.

  Facts worth carrying into the rewriter:
  - Library resource handles come from `createHandleForLib` (160) applied to a
    loaded global, NOT `createHandle` (57) applied to a binding index. So the
    resources have to become real globals and `!dx.resources` has to point at
    them instead of `undef`.
  - **The CFG needs no changes at all**, phi nodes included. `CommittedStatus`
    was already compared against `COMMITTED_TRIANGLE_HIT` (1), and the
    generated `ClosestHit` writes `hit = 1` while `Miss` writes `0`, so loading
    the payload's hit field is the identical test.
  - **Exports need no MSVC mangling.** DXC emits `\01?RayGen@@YAXXZ`, but plain
    `@RayGen` works and `RDAT` exposes it under exactly that name.
  - LLVM numbers unnamed values and blocks POSITIONALLY. Added values must be
    named, and deleting a numbered value breaks the sequence outright: the
    assembler refuses with "instruction expected to be numbered '%33'". Do not
    work around this per site. Run `phase5/hand/llnorm.py` first, which renames
    `%33` to `%v33` and block 44 to `bb44` throughout, after which nothing is
    positional and blocks can be added or deleted freely. Verified safe on its
    own: normalising and reassembling an untouched module still validates and
    signs. **The rewriter should start with this pass.**

  Pattern 3, `phase5/hand/make_lib_alpha.py`, is the one the project rests on,
  since it has the rotated `Proceed` loop that pattern 1 lacks entirely.
  8117 of 65536 rays hit on both WARP and the lowered version, 0 mismatches.
  The lowering deletes SIX basic blocks from the raygen (preheader, header,
  candidate block, latch, commit block, exit) and replaces them with a payload
  init and one `traceRay`. The any-hit body is the loop body re-rooted onto the
  `attr` parameter, with the arithmetic completely unchanged.
  - **The polarity inverts.** Committing is the special path in RayQuery;
    `IgnoreHit` is the special path in any-hit. Accept is the fall-through in
    one and the explicit case in the other. Easiest thing here to get backwards.
  - **The `CandidateType()` test is dropped**, because it is a tautology inside
    an any-hit shader. A `CANDIDATE_PROCEDURAL_PRIMITIVE` arm would have to
    become an intersection shader instead. Not exercised, not tested.
  - `IgnoreHit` is `noreturn nounwind` and a compute module has no attribute
    group for it, so one has to be appended.

  Next action: automate. Both scripts key off exact instruction text, which is
  fine for a fixed input and useless for a real one. The rewriter has to find
  the same sites structurally: follow the query handle, match opcodes on
  operand 0, recognise the rotated loop. `llnorm` first, `dxilrt asm` as the
  feedback loop, `raytest --lib` as the correctness check. Pattern 2, shadow
  rays with `ACCEPT_FIRST_HIT_AND_END_SEARCH` and a miss shader only, is not
  yet attempted and should be the easiest of the three.

  Still untested, and all of it matters before this meets a real shader:
  descriptor tables rather than root descriptors, more than one query per
  shader, procedural primitives and the intersection path, and every
  no-valid-lowering case in the "Cases with no valid lowering" table below.
  Everything proven so far is one compute shader with three root-level
  bindings and a single query.

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

  The device wrapper is `Dxr11Device` in proxy/d3d12_device.{h,cpp}: all 65
  ID3D12Device7 methods forwarded unchanged. Device6 and Device7 are optional,
  QueryInterface returns E_NOINTERFACE when the real device lacks them. It
  wraps only the device, not child objects, so ID3D12DeviceChild::GetDevice()
  still returns the real device; Phase 4 has to handle that if an app trips on
  it. QueryInterface above Device7 is passed through unwrapped AND logged;
  nothing in the current test apps does that except ID3D12InfoQueue, which is
  correct. DXR11_NO_WRAP=1 disables wrapping, restoring forward-only
  behaviour.

Dev machine (Windows x64), everything under `C:\DW`:
- `C:\DW\dxr11-pascal` - this repository.
- `C:\DW\DXC` - DirectX Shader Compiler (`inc\dxcapi.h`, `bin\x64\dxcompiler.dll`,
  `dxil.dll`).
- `C:\DW\microsoft.direct3d.d3d12.1.619.5` - Agility SDK.
- `C:\DW\DirectX-Graphics-Samples` - Microsoft samples, source of the DXR 1.0
  app used to validate the proxy.
- Build scripts: `build.bat` (phase 1), `build_phase2.bat`, `build_proxy.bat`,
  `build_sample.bat`, `build_phase4.bat`.
  `build_phase4.bat` builds the Tier 1.1 probe into `phase4out\`, a directory
  with no proxy in it so the probe measures the real runtime; copy `d3d12.dll`
  in to measure the shim instead. `tier11probe.exe [warp|hw]` runs the three
  feature probes, and takes `-debug` for the debug layer, `-gfxsplit` for
  graphics state across a split, `-batchsplit` for dispatch batching and its
  control, `-time` and `-pipeline` for the split cost.
  `build_phase5.bat` extracts the Phase 2 shaders and disassembles them into
  `phase5\dxil\`; `build_phase5_tool.bat` builds `phase5out\dxilrt.exe`, which
  round-trips a container through text and assembles and signs arbitrary `.ll`.
  Both outputs are generated and gitignored.
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

### Vulkan draws the same line, which is the best evidence we have

Checked on the dev machine, driver 582.66 (June 2026), GTX 1070, Vulkan 1.4,
with `vulkaninfo`:

    VK_KHR_acceleration_structure      revision 13
    VK_KHR_ray_tracing_pipeline        revision 1
    VK_KHR_ray_tracing_maintenance1    revision 1
    VK_NV_ray_tracing                  revision 3
    VK_KHR_ray_query                   ABSENT

Pipeline ray tracing yes, ray query no. That is the same cut NVIDIA makes in
D3D12, DXR 1.0 yes and Tier 1.1 no, arrived at through a completely unrelated
API on the same silicon. Exposing `ray_tracing_pipeline` without `ray_query` is
not a normal desktop combination, so it is a deliberate carve-out rather than a
capability boundary.

REPORTED, NOT VERIFIED: driver 460.89 (December 2020) is said to have exposed
`VK_KHR_ray_query` on Pascal, and to be the only driver that did. If true, then
NVIDIA had inline queries running on this hardware and later withdrew them,
which is a strong form of the premise above. Confirming it needs a driver
rollback, which would break this project's dev environment, so it stays
unverified for now.

### Vulkan is not a route, only evidence

Reaching `VK_KHR_ray_query` from a D3D12 shim means translating D3D12 to
Vulkan, which is vkd3d-proton, not a variant of this project. None of this
repository would carry over, since the export ordinals, the device wrapper and
the command list split are all D3D12 plumbing. Worse, acceleration structures
cannot be shared between the two APIs, so every BLAS and TLAS would be built
twice, which breaks the premise that rays hit NVIDIA's own AS traversed by
NVIDIA's own code.

Running an application under vkd3d-proton on driver 460.89 would in principle
hand over DXR 1.1 including inline ray tracing for free. INFERRED, not tested:
this probably eats itself, because a vkd3d-proton recent enough to implement
DXR 1.1 wants Vulkan 1.3 and a long extension list, while a December 2020
driver is Vulkan 1.2 era. The driver old enough to expose `ray_query` is likely
too old for the translation layer that could use it. The same age problem hits
Unreal from the other side: UE5 ships its own Agility SDK and expects far more
of the D3D12 runtime than a 2020 driver provides.

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
`tools\run_proxy_test.ps1 -Exe ... [-Animated]` runs one with and without the
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

**Probe result (2026-09-21), docs/phase4-probe.md.** `phase4/tier11probe.cpp`
measures all three on WARP (Tier 1.1 ground truth) and on the 1070:
- **Ray flags need no work.** `SKIP_TRIANGLES` and `SKIP_PROCEDURAL_PRIMITIVES`
  behave identically on both adapters, on a scene with both triangle and
  procedural geometry. The Tier 1.0 driver honours them.
- **Indirect DispatchRays** stops at `CreateCommandSignature`. ByteStride is
  104. A device method, so the wrapper already has the seat.
- **AddToStateObject** stops earlier than expected, at `CreateStateObject`,
  which rejects `ALLOW_STATE_OBJECT_ADDITIONS` (flags 0x4). Needs two
  interception points, and an addition must repeat the shader config, the
  pipeline config and the config flag; those are NOT inherited by new exports.
- `ID3D12Device7` **is present on the 1070**, so `AddToStateObject` is callable
  there and the wrapper must cover Device7 before any of this works.

**Result: all three features PASSED (2026-09-21).** Ray flags needed no shim.
AddToStateObject is emulated by strip, cache and rebuild,
docs/phase4-addtostateobject.md. Indirect DispatchRays is emulated by splitting
the command list around the dispatch, docs/phase4-indirect-design.md. Each is
checked against WARP as ground truth, and the regression suite is raytest ALL
MATCH, the probe on hardware matching on both argument-buffer shapes, and both
Microsoft samples unchanged with the debug layer clean.

The estimate of the engineering cost was right: command list splitting was
the bulk of it, and preserving binding state across the break was the part that
kept being subtly wrong.

**Still open, on purpose:** `CheckFeatureSupport` reports Tier 1.0. Reporting
Tier 1.1 entitles the app to emit RayQuery, so until phase 5 works, either keep
reporting 1.0 or detect RayQuery DXIL and fail clearly. A shim that claims 1.1
and then crashes is worse than one that claims 1.0.

### Phase 5: the DXIL rewriter

Automate phase 2's transform. Detect `dx.op.rayQuery_*` opcodes, generate the
raygen/any-hit/closest-hit/miss set, synthesize the state object and shader
table, re-sign.

**Recon result (2026-09-21), docs/phase5-dxil-recon.md.** The original plan
here said "DXIL is LLVM 3.7 bitcode" and budgeted months for parsing and
emitting it. Measurement says that is not necessary.

`IDxcAssembler::AssembleToContainer` turns `.ll` text back into a DXIL
container, and Phase 1 already established that `dxil.dll` signs anything that
validates. `phase5/dxilrt.cpp` measures the round trip. All four Phase 2
shaders survive it with every instruction and metadata line identical; the
entire diff is comment lines.

- **Lost:** `STAT` shrinks, so reflection resource names go empty and struct
  layouts become `[32 x i8]`, and `VERS` is dropped. An app that calls
  `ID3D12ShaderReflection` on a shader we rewrote would see blank names.
  Nothing tested so far does that. Known limitation, recorded on purpose.
- **Preserved byte-identical:** `PSV0` and, critically, `RDAT`, which is what
  `CreateStateObject` reads to find a library's exports. Had that been damaged
  the whole approach would be dead.
- **Proven on hardware, not merely signed:** `raytest.exe --roundtrip` reports
  ALL MATCH, 14450 and 8117 hits. `--rtpoison` makes one text edit to the ray
  direction and gives DIVERGE, so the harness genuinely sees IR changes. Note
  the first version of that check poisoned BOTH sides and reported MATCH, which
  proved nothing; it has to be asymmetric.

So the route is to **edit the input module in place as text**: change
`!dx.shaderModel` from `cs` to `lib`, rename `main` to a mangled raygen export,
replace the rayQuery ops with a `traceRay` call, append three small functions,
and rewrite `!dx.entryPoints` with `!dx.typeAnnotations`. The application's own
code never moves between modules, so its resource bindings and handles cannot be
got wrong in transit. Emitting a container from scratch is pointless when the
input module already has the right resources, handles and types.

**Patterns 1 and 3 both work by hand (2026-09-21).** `phase5/hand/make_lib.py`
and `make_lib_alpha.py` lower `rayquery_opaque.ll` and `rayquery_alpha.ll` into
`lib_6_5` libraries as text. Both render bit-exactly against WARP on the 1070,
14450 and 8117 of 65536 rays, 0 mismatches either way. Pattern 3 is the real
one: its `Proceed` loop becomes a generated any-hit shader.

The method that worked, and worth repeating: change ONE thing, the shader
model, and let the validator enumerate the rest of the job.

Key facts for the transform, all read off real DXC 1.10 output:
- `AllocateRayQuery` is 178, confirmed. But **dispatch on the opcode immediate,
  not the callee name**: 184 and 185 are the same LLVM function, as are 193 and
  194, so name matching silently confuses committed with candidate accessors.
- The RayQuery object is a plain `i32` SSA value, not memory.
- `while (q.Proceed())` is **loop-rotated**, with two `Proceed` calls where the
  source has one. Nothing looking for "a loop whose condition is Proceed" will
  find it.
- `dx.op.threadId` (93) is not legal in a raygen and must become
  `dx.op.dispatchRaysIndex` (145).

Reference implementations to read before writing anything. Note their front
ends matter less now that the text level is available:
- **dxil-spirv** (HansKristian-Work): a DXIL parser with its own lightweight
  LLVM bitcode reader, already handles ray tracing opcodes. Its front end is
  the reusable half; it emits SPIR-V, not DXIL, so the back end is not.
- **Maister's "My personal hell of translating DXIL to SPIR-V"**, parts 1-4.
- **Mesa RADV** `radv_nir_lower_ray_queries.c`: the equivalent lowering at NIR
  level, for the algorithm.
- **Microsoft's archived D3D12 Raytracing Fallback Layer**: architecture and
  its documented limitations. Note it never supported inline ray tracing.
- **DXIL specs** for the `dx.op.rayQuery_*` opcode list. Microsoft appends
  opcodes over shader model revisions, so the numbers need verifying against
  the DXC in use. Note `DxilConstants.h` is NOT in the DXC redist at
  `C:\DW\DXC\inc`, which ships only the API headers. Easier: the disassembler
  prints the opcode name as a trailing comment, so `dxc -Fc` on a shader that
  uses the opcode gives the number directly. That is how the table in
  docs/phase5-dxil-recon.md was built.

## Testing

WARP is the oracle throughout. It implements Tier 1.1 correctly in software,
so any RayQuery shader can be run there for ground truth and diffed against
the lowered version on the 1070. No RTX card needed.

`raytest.exe` carries the Phase 5 hooks:
- `--lib <file.dxil>` makes the TraceRay side load a pre-built library from
  disk instead of compiling HLSL, so anything the rewriter produces can be held
  against the same ground truth.
- `--roundtrip` sends every shader out to `.ll` text and back before D3D12 sees
  it. `--rtpoison` does the same but corrupts the lowered side on purpose, and
  must DIVERGE.

Check sensitivity before believing a pass. Twice in this project a test passed
for the wrong reason: the `-gfxsplit` control re-bound a PSO the test had
abandoned, and the first `--rtpoison` corrupted BOTH sides so they still agreed
with each other. A test that cannot fail has not been run.

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
