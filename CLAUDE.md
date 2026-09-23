# pascal-dxr-tier-1.1: DXR Tier 1.1 compatibility shim for NVIDIA Pascal

Brief version 1. Not the project's version: that lives in CHANGELOG.md and
proxy/version.h, and is currently 0.37.0.

## Current position (2026-09-23)

**THE ESCHER GPU CRASH HAS A CAUSE AND A FIX (0.37.0), NOT YET RUN IN THE
GAME.** A hit shader reading a local root signature cbuffer crashes the Pascal
driver inside `CreateStateObject`. That was how the shim delivered
`GeometryIndex` and `InstanceContributionToHitGroupIndex`; they are now baked
into a copy of the hit shaders per (geometry, contribution) pair. The fifteen
Unreal libraries that crashed are 0 of 225 offline. The next step is an Escher
run. See the driver crash entry below.

**THE GOAL IS REACHED.** A RayQuery compute shader, unmodified, runs on the
GTX 1070 and produces bit-exact output against WARP. 24 render cases, 0
mismatches, plus four gates. Run `.\tools\run_dispatch_test.ps1`.

The premise held: Pascal already traces rays, and only the Tier 1.1 API surface
was missing. Nothing in this project implements ray tracing.

**ALL THE LOWERING IS DONE, AND ALL 25 ACCESSORS NOW WORK.** The four that
were called permanently impossible are not. `Candidate`/`CommittedGeometryIndex`
and `*InstanceContributionToHitGroupIndex` were written off while the
APPLICATION owned the shader table; the shim builds it, so each record carries
those numbers as local root signature constants and the hit shader reads them
back. Measured SFI0 = 0x0, which was the gate.

**A REAL UNREAL SHADER NOW LOWERS, VALIDATES AND SIGNS.**
`RayTracingDebugMainCS`, dumped from a running UE 5.8.2 game, goes DXIL
container in and signed container out. That is the first time anything not
written for this project has been through the whole path.

**The shader table is now built from the application's own geometry layout.**
Acceleration structure interception is complete and the instance data is USED.
The hit group table is sized to the scene's largest
`InstanceContributionToHitGroupIndex`, and each record carries the geometry
TYPE that reaches that index, so a scene holding both triangles and procedural
primitives works rather than being refused.

**EVERY RayQuery SHAPE THIS PROJECT SET OUT TO SUPPORT NOW WORKS**, including
a query that commits BOTH triangle and procedural hits, which lowers to an
any-hit AND an intersection shader from one Proceed loop.

**WHAT IS LEFT IS A SHORT LIST, AND TWO OF THEM ARE THE REWRITER'S OWN.**
Ordered by what a real engine actually hits:

1. **Multiple RayQuery objects in one entry point.** 33 of Unreal's shaders,
   and **the refusal is FALSE**: the check counts ALLOCATIONS, not liveness,
   and the two queries in NiagaraCollisionRayTraceCS are three hundred lines
   apart. Lowering them is real work, one TraceRay and one hit shader set per
   query, but "no lowering exists" was never true. THIS IS THE NEXT BUILD.
2. **One assembler failure** left in Unreal's log, cause not yet identified.
   The dump is the way in, not more reasoning.
3. **A 6.6 binding into a resource ARRAY at a dynamic index.** The 6.5 path
   supports exactly this. One reference compile settles the shape.
4. **RayQuery hosted in a raygen shader**, which is what both NVIDIA samples
   do. Lowerable in principle, because a raygen CAN call TraceRay; the
   obstacle is shader table OWNERSHIP, not the shaders.

Genuinely refused, and each is a fact about DXR 1.0 or about what the
APPLICATION did, not a gap:

- RayQuery in a pixel, vertex, mesh or amplification shader;
- RayQuery inside an any-hit or intersection shader, which cannot call
  TraceRay (a raygen is a different case, see 4 above);
- groupshared memory or wave intrinsics around the query;
- a loop body reading genuine caller locals that do not fit the payload;
- an application routing both geometry kinds to ONE hit group record.

So the question is still **WHAT TO RUN**, not what to build: real software,
enough of it that the refusal list is trusted. That is what the version number
tracks, see CHANGELOG.md: 0.x until real software has been through it, 1.0.0
when the refusal list is trusted.

See the entries below and docs/phase5-dxil-recon.md.

**IT IS SHIPPED NOW**, and that changed several defaults. Repository
`pascal-dxr-tier-1.1`, branch `main`, tagged v0.12.0, with README.md,
CHANGELOG.md, docs/usage.md and a setup GUI. The rules below are about being a
thing somebody downloads, and they are not the same as the rules for a thing
only its author runs:

- **Tier 1.1 is ON by default.** The install IS the opt-in: a proxy DLL only
  sits beside an executable because somebody put it there. Requiring a second
  opt-in through an environment variable that Steam and the Epic launcher never
  pass on mostly produced reports that the shim does nothing. The old gate was
  right for development, where the risk was a test run silently flipping, and
  wrong for a release.
- **Settings come from `dxr-tier-11.ini` beside the DLL**, because a file is the only
  mechanism that works regardless of how an application is launched. The
  environment still WINS over the file, deliberately, so a stray `.ini` can
  never change what the test scripts measure. See proxy/config.h.
- **Without DXC the shim stands aside entirely.** It neither claims Tier 1.1
  nor wraps the device. Two reasons, and the second is the one worth
  remembering:
  - Claiming Tier 1.1 is a promise to translate RayQuery, and the promise
    cannot be kept without `dxcompiler.dll` and `dxil.dll`. Copy the DLL alone,
    forget the other two, and an application is told it may emit RayQuery, does
    so, and the driver rejects a shader nobody could rewrite. That is the one
    failure this project refuses to ship, and making the tier claim the default
    had introduced it.
  - Everything the WRAPPER still does is a Tier 1.1 feature, so a Tier 1.0
    application never calls any of it. Wrapping would be risk with no benefit,
    and the wrapper hooks a queue vtable and wraps every command list. The cost
    of checking: DXC now loads at device creation rather than at the first
    shader, so the lazy loading noted below is gone when DXC is present.
- The log prefix is `[dxr-tier-11-proxy-log]` and it names the version and the
  DXC it loaded, because a bug report arrives as a log file from a binary
  nobody can identify.

**UNREAL REFUSES PASCAL BY PCI DEVICE ID, AND THE TIER HAS NOTHING TO DO WITH
IT.** This is the most consequential external fact the project has found, and
it was found by running a real game rather than by reading anything.

`Engine/Source/Runtime/D3D12RHI/Private/Windows/WindowsD3D12Device.cpp`,
verified in stock UE 5.7.4:

    static bool IsRayTracingEmulated(uint32 DeviceId)
    { ... 0x1B81, // "NVIDIA GeForce GTX 1070" ... }

    if (GRHISupportsRayTracing && IsRayTracingEmulated(AdapterDesc.DeviceId))
    {
        DisableRayTracingSupport();
        UE_LOG(..., TEXT("Ray tracing is disabled for NVIDIA cards with the Pascal architecture."));
    }

It runs AFTER the tier check. The shim reports 1.1, Unreal sets
`GRHISupportsRayTracing`, then reads the device id and turns it all back off.
**No cvar, no command line flag, no config guards it**: the
`GAllowEmulatedRayTracing` escape hatch older versions had was removed. Grepped
for it, for `ForceEnableRayTracing` and for `AllowEmulated`; nothing.

So `proxy/dxgi_spoof.{h,cpp}` and a second proxy DLL, `dxgi.dll`, report a
Pascal card under a Turing device id. See docs and the CHANGELOG. Facts worth
carrying:

- **DXGI adapters SHARE one vtable and it lives in the module image.**
  Measured, not assumed, because this project has been wrong both ways before:
  queues share, lists get per-object heap vtables. `IDXGIAdapter4` is the same
  object with the same table, so `GetDesc`, `GetDesc1`, `GetDesc2` and
  `GetDesc3` are four slots in one place and cover every adapter from every
  factory. **Nothing is wrapped**, so `D3D12CreateDevice` keeps receiving the
  real object and there is no unwrapping problem.
- **It cannot be done from d3d12.dll.** Unreal calls `IDXGIAdapter::GetDesc`
  BEFORE its first `D3D12CreateDevice`, so no d3d12 export is early enough. The
  hook installs from `CreateDXGIFactory*`, which is the last moment before the
  first adapter can exist.
- **Exactly one field changes**: `DeviceId`, on the cards Unreal itself lists.
  NOT the vendor id, so NVAPI still runs. NOT the description, because
  `FWindowsPlatformMisc::GetGPUDriverInfo` matches the description STRING
  against `EnumDisplayDevices` and lying there breaks a real lookup for nothing.
- **Every capability answer stays true.** Unreal asks NVAPI for shader
  execution reordering, cluster ops, atomic64 and the driver version, and all
  of those reach the real device. SER correctly reports unsupported. Unreal
  never asks the hardware what it IS, only what it can DO, and only the first
  question is answered differently.
- The default id is 0x1F08, an RTX 2060: lowest-end Turing, and Turing rather
  than Ampere because Unreal carries a separate `IsNvidiaAmpereGPU` list.
- The install self-tests all four slots on a real denied adapter and rolls back
  every slot as a unit if any check fails.

**What was ruled out first, and each is worth not re-deriving.** The GTX 1070
passes EVERY hardware gate Unreal has, measured on the real device: feature
level 12_1, shader model 6.8, resource binding tier 3, wave ops, and 64-bit
typed atomics on typed resources. Atomic64 was the expected blocker and is not
one. The game ran on SM6, proven by `<Project>_PCD3D_SM6.upipelinecache` being
written during the run. And cooking is host-independent:
`ShouldCompileRayTracingShadersForProject` reads `GRayTracingPlatformMask`,
filled from `TargetPlatformSettings->GetRayTracingShaderFormats()`, so building
a game on a Pascal machine does NOT omit the ray tracing shaders.

**One log line looked like proof and was not.**
`CreateCommandSignature(DISPATCH_RAYS)` is keyed off
`GRHISupportsRayTracingDispatchIndirect`, which comes from the TIER alone, not
from `GRHISupportsRayTracing`. It showed the tier was believed, nothing more.
The device wrapper's startup `caps:` lines now report Unreal's own SM6 gate for
exactly this reason: so the caps branch is ruled out in one line rather than an
afternoon.

**`fopen_s` and `_wfopen_s` open EXCLUSIVELY.** `d3d12.dll` opened the log
first and `dxgi.dll` then could not, so a demonstrably loaded DLL logged
nothing at all. A silent component looks exactly like one that never ran. Both
use `_wfsopen` with `_SH_DENYNO` now, and logging lives in
`proxy/proxy_log.cpp`, linked into both DLLs.

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

- Phase 5 rewriter: **AUTOMATED, and it matches both hand lowerings.**
  `phase5/rewriter/` finds the sites structurally, with no input-specific
  text. `.\tools\run_rewriter_test.ps1` runs it end to end, taking ground
  truth from WARP every run rather than from a stored baseline.

      pattern 1, opaque closest hit                14450 hits   MATCH
      pattern 3, alpha-tested, generated any-hit    8117 hits   MATCH
      resource array indexed [2], via a table      14450 hits   MATCH
      descriptor-table root signature, no array    14450 hits   MATCH
      independent shader, resource read in loop     6333 hits   MATCH
      CommittedInstanceIndex + PrimitiveIndex      18496 hits   MATCH
      Abort(), order-independent observable only               MATCH
      procedural primitives, generated intersection            MATCH
      the 11 accessors from the Unreal survey                  MATCH
      commits BOTH kinds, two hit groups            9160 hits   MATCH
      resource array at a DYNAMIC index            14450 hits   MATCH
      the same through NonUniformResourceIndex     14450 hits   MATCH

  All bit-exact, 0 mismatches, max |dt| and max |dbary| 0.000000.
  Those twelve were the suite at the time. It now runs THIRTEEN render cases,
  each also checked byte-identical between the Python and the C++ on both the
  `.ll` path and the container path, and fifteen analysis and lowering checks,
  by `.\tools\run_rewriter_test.ps1`.

      dxil.py        small .ll model: blocks, instructions, dx.op decoding,
                     dominators and natural loops
      rayquery.py    finds and classifies the query, refuses what has no lowering
      lower.py       the transform, driven entirely by the analysis
      dxrewrite.py   CLI: analyze <in.ll> | lower <in.ll> <out.ll>
      test_reject.py provokes each refusal by mutating a known-good input

  All three structural handles the recon promised held up, which is the main
  result: the query handle is a plain `i32` so ownership is one def-use hop;
  dispatching on the opcode immediate avoids confusing CommittedStatus (184)
  with CandidateType (185), which share one LLVM function; and dominator-based
  loop detection reports exactly the header, latch and body the recon worked
  out by hand. No textual pattern finds that loop.

  **Opcodes are a whitelist** of the ones actually observed in DXC output,
  nine at first and 26 now that the Unreal accessors are covered.
  Anything else stops the lowering. Given two of them share one LLVM function,
  a near miss would render the wrong thing silently.

  Pattern 2 also lowers and renders correctly, from an input built by setting
  `ACCEPT_FIRST_HIT_AND_END_SEARCH` on the opaque shader. CAVEAT: this scene
  has one triangle layer, so accept-first-hit is indistinguishable from
  closest-hit in it. The path works; first-hit semantics are not proven.

  **Descriptor tables: tested, and the worry was misplaced.** This brief and
  the docs both named them as the most likely thing to break the rewriter.
  They are not, and the test found the right thing instead.
  - A descriptor table alone changes NOTHING in the DXIL. Measured against the
    root-descriptor original: byte-identical function body, identical resource
    records. The root signature lives in the container's `RTS0` part, not in
    the module, and a non-library shader reaches resources through
    `createHandle(rangeId, index)` whatever the root signature says.
  - The real hazard is resource ARRAYS, which tables enable. `outBufs[4]`
    indexed at `[2]` gives `createHandle` with index 2 and a `[4 x T]` type,
    and the lowering was reading the rangeId while ignoring the index. It did
    not render wrong only by luck, and the analysis had already accepted it.
  - Now supported, in the form DXC itself uses for a library: the global
    carries the array type, the element comes through a constant
    `getelementptr`, and `createHandleForLib` takes the ELEMENT type. See
    `phase5/cases/reference/`, which exists so such questions get answered by
    DXC rather than by reasoning.
  - `raytest --table` binds a real four-descriptor table where only slot 2
    points at the output and 0, 1 and 3 point at a decoy, so a dropped index
    writes nowhere visible.

  **An independently written shader found a false refusal, on the first try.**
  `phase5/cases/rayquery_indep.hlsl` is deliberately not descended from the
  Phase 2 pair: 16x16 group, the query behind a helper function, `continue`
  inside the loop, accessors in a different order, and a RESOURCE READ INSIDE
  THE PROCEED LOOP, which is what a real alpha test does. 6333 of 65536 rays,
  bit-exact against WARP running the same shader.
  - The loop isolation check refused it, because the `alphaMask` handle is
    created outside the loop and used inside. Right in principle, wrong here:
    **a resource handle is not caller state.** It names a resource, and every
    shader in the library can reach the same one.
  - This mattered. Refusing it would have blocked the most common real alpha
    test there is, the one that samples a texture or buffer per candidate. The
    Phase 2 pair could never have exposed it, because its alpha rule is pure
    arithmetic on the barycentrics.
  - Fixed: `createHandle` results are exempt from the isolation check, and the
    any-hit recreates each handle the body uses under the SAME SSA name it had
    in the raygen, so the transplanted instructions need no rewriting. Genuine
    caller locals are still refused.
  - `raytest --cs <file.hlsl>` compiles a given shader for the ground-truth
    side, so any independent shader can be its own oracle through WARP.

  **A second independent shader found a Tier 1.1 feature with NO lowering.**
  `phase5/cases/rayquery_ids.hlsl` uses the committed index accessors.
  `CommittedInstanceIndex` (207) and `CommittedPrimitiveIndex` (210) now work,
  mapping to `InstanceIndex()` (142) and `PrimitiveIndex()` (161).
  **`CommittedGeometryIndex` (209) does not and cannot**, see the
  no-valid-lowering table below. All five of those opcodes share the one
  `rayQuery_StateScalar.i32` function, which keeps vindicating dispatch on
  operand 0.
  - **The payload size is part of the state object contract.** Growing it from
    16 to 28 bytes broke `CreateStateObject` with `E_INVALIDARG` until
    `MaxPayloadSizeInBytes` was raised to match. Whatever generates the shaders
    does not get to pick the payload freely; `PAYLOAD_BYTES` in `lower.py` and
    the shader config have to move together.
  - `raytest --multi` builds two instances of a two-triangle quad, so instance
    varies left to right and primitive across the diagonal. Against the default
    one-triangle scene every correct answer is 0 and a lowering returning a
    constant would pass.

- RayQuery detection in the proxy: **DONE.** `proxy/dxil_scan.{h,cpp}`, hooked
  at `CreateComputePipelineState`, `CreatePipelineState` and
  `CreateStateObject`. It looked like it needed a bitcode parser in the proxy.
  It does not: **SFI0 bit 20 is the whole signal.** Measured across every
  shader in phase5, a plain compute shader reads 0x0, every RayQuery shader
  reads 0x100000, a DXC-built DXR 1.0 library reads 0x0, and the rewriter's own
  output reads 0x0. SFI0 is an eight byte container part, so detection is a
  container walk and a mask with no LLVM involved.
  - The rewriter's OUTPUT reading 0x0 matters on its own: the lowered library
    correctly stops claiming Tier 1.1, which is what lets the driver accept it.
  - Detection deliberately does NOT change what is forwarded. On Tier 1.0 the
    driver rejects these shaders anyway, and replacing its error with ours
    would hide information. The log is the clarity.
  - Verified both ways: raytest reports its RayQuery shader at exactly 4788
    bytes, while the DXR 1.0 probe and HelloWorld stay SILENT, so DXR 1.0
    libraries do not false-positive.

  That fork is now DECIDED and the port is done, see below.

- The C++ port: **DONE, and byte-identical to the Python.**
  `proxy/rewriter/` is the analysis and the lowering in C++, 1689 lines against
  the Python's 1303, built into `phase5out\dxrw.exe` by `build_rewriter.bat`.
  `ll_model` ports parsing, CFG, dominators and natural loops; `rq_analyze`
  ports find, classify and refuse; `rq_lower` ports the transform.

  **The bar was byte-identical output, not "the tests still pass."** A port is
  exactly the case where passing tests is too weak: both implementations are
  checked against the same six cases, so a shared misunderstanding passes
  twice. All six match exactly, and the refusals agree message for message. The
  comparison is part of `.\tools\run_rewriter_test.ps1`, so the two cannot
  drift apart quietly.

  It was sensitive on its first run, and what it caught was a bug in the
  PYTHON: `_append_shaders` used `re.search(r'^\}\s*$', text, re.M)`, and in
  multiline mode `\s*` also eats the following newlines, so the insertion point
  drifted and produced a run of blank lines nobody intended. Both sides now do
  it on lines.

  Two things MSVC forces, worth knowing before touching this code:
  - **`std::regex` has no multiline mode at all.** Every pattern that used one
    became a line operation, which is clearer, and is what exposed the bug
    above.
  - **A raw string ending in `)"` terminates early.** `R"( ... !"(\w+)" ... )"`
    is not the string it looks like. Use `R"RX( ... )RX"` when the pattern
    contains a quote.

- The DXC host: **DONE.** `proxy/rewriter/dxc_host.{h,cpp}` closes the gap
  between what the rewriter works on, `.ll` TEXT, and what D3D12 hands over, a
  DXIL CONTAINER. Disassembly and assembly from `dxcompiler.dll`, signing from
  `dxil.dll`, the Phase 1 mechanism unchanged. `dxrw rewrite <in.dxil>
  <out.dxil>` is the whole path in one call and is what the proxy will do
  internally; all five RayQuery shaders go container in, signed container out,
  and render bit-exactly on the 1070. The container path is its own line in the
  regression, separate from the `.ll` path, because it is the one that will
  actually run in an application.

  Three things a DLL in someone else's process has to get right, all handled
  and worth not undoing:
  - **Load DXC by FULL PATH, never by name.** An application may already have
    its own `dxcompiler.dll` loaded, and `LoadLibraryW(L"dxcompiler.dll")`
    returns THEIRS, of whatever version. `SetHostModule` is called from
    `DllMain` so the host can find the copies beside the shim. Proven: with the
    beside-exe copy hidden and a different one present in the working
    directory, it refuses rather than loading the wrong one.
  - **Never throw.** Every failure is a returned error; a missing DLL is a
    message, not a fault in the host process.
  - **Load lazily and once.** HelloWorld runs through the proxy with the whole
    rewriter linked in and loads no DXC at all, still pixel-identical.

- The dispatch path: **DONE. RayQuery compute shaders run on the 1070.**
  `proxy/rq_pipeline.{h,cpp}`. What the shim substitutes:

      CreateComputePipelineState  lowers the DXIL, builds a state object and
                                  shader table, returns a stand-in
      SetPipelineState            recognises the stand-in, never forwards it
      Dispatch(gx,gy,gz)          SetPipelineState1 then DispatchRays

  `Dxr11RayQueryPso` is an `ID3D12PipelineState` the application holds and
  never inspects, the same shape as `Dxr11CommandSignature` and for the same
  reason.

  **What made this tractable was a fact, not a technique: DXR's global root
  signature IS the compute root signature.** `SetComputeRootSignature` and
  `SetComputeRoot*View` are exactly what `DispatchRays` consumes, so the
  application's bindings carry across with NO translation, and the compute
  PSO's root signature simply becomes the state object's
  `GLOBAL_ROOT_SIGNATURE`. Most of the feared difficulty was not there.

  The one real conversion: `Dispatch` counts thread GROUPS, `DispatchRays`
  counts RAYS. The lowered raygen reads `DispatchRaysIndex` where the original
  read `SV_DispatchThreadID`, so the ray grid is groups times `numthreads`,
  read from the entry point's properties (tag 4) before the lowering strips
  them. The overhang is harmless: a shader that bounds-checked its threads
  bounds-checks its rays identically. **The `numthreads(16,16,1)` case is in
  the regression precisely because everything else is 8x8** and a hardcoded
  group size would have passed every other test.

- Coverage against a real engine: **SURVEYED, and the gap is large.**
  - **No Microsoft sample uses RayQuery.** All eleven D3D12Raytracing samples
    are DXR 1.0; the apparent "inline" matches are the C++ keyword. They can
    prove the shim is TRANSPARENT and can never prove the rewriter is RIGHT.
  - **Unreal Engine 5.7** is the real RayQuery code available, nine files under
    `Engine/Shaders/Private`. They cannot be compiled standalone, so this is a
    SURVEY, not an execution test.
  - **Epic uses 25 accessors; the rewriter supports 8.** Nothing of Epic's
    would lower today. The biggest gaps, `CandidatePrimitiveIndex` and
    `CandidateInstanceIndex`, are used 11 times each, as often as
    `CommittedRayT`.
  - Reassuring: **every use is in a COMPUTE shader.** The pixel-shader case,
    which this brief calls the most consequential of the no-lowering cases, is
    not hit by Unreal at all. Epic's template flags all pass straight through.

  The cost of the gap is MEASURED, not guessed, because `GeometryIndex` already
  proved an intrinsic can look ordinary and be secretly Tier 1.1. Every
  proposed mapping was compiled and its SFI0 read, see
  `phase5/cases/reference/lib_accessors_ref.hlsl`. The 16 unsupported accessors
  split:
  - **11 mechanically addable.** `PrimitiveIndex`, `InstanceIndex`,
    `InstanceID`, `HitKind`, `RayTCurrent`, `ObjectRayOrigin`,
    `ObjectRayDirection`, `WorldToObject4x3` all measure SFI0=0x0.
    **The SAME intrinsic serves the Candidate and Committed forms**, because in
    an any-hit shader `PrimitiveIndex()` IS the candidate and in a closest-hit
    it IS the committed hit. One table covers both.
  - **2 permanently blocked, Tier 1.1**: Candidate/CommittedGeometryIndex.
  - **2 permanently blocked, no HLSL intrinsic at all**:
    `*InstanceContributionToHitGroupIndex`.
  - **2 structurally larger**: `CommitProceduralPrimitiveHit`, needing the
    generated intersection shader that has never been built, and `Abort()`.

  Not settleable by survey: several Epic files declare more than one `RayQuery`
  object, one declares five. Whether those are concurrent within an entry point
  after inlining, which the rewriter refuses, needs the compiled DXIL and so
  needs the engine.

- The 11 accessors: **DONE, coverage 8 of 25 becomes 19 of 25.** Added to both
  implementations, byte-identical, and render-verified on the 1070 by
  `phase5/cases/rayquery_acc.hlsl`.
  - The operand shape is IDENTICAL on both sides apart from the query handle,
    which simply goes away: `StateMatrix(op, handle, i32 row, i8 col)` becomes
    `worldToObject(152, i32 row, i8 col)`. That is why eleven accessors cost
    one small table rather than eleven special cases.
  - Two need real conversion. `*TriangleFrontFace` returns `i1` in RayQuery
    while `HitKind()` is an integer, so `icmp eq i32 hk, 254`. The
    world-to-object matrix is twelve floats and travels in the payload as
    `[12 x float]` indexed `row * 4 + col`.
  - **The payload is now 88 bytes**, almost all matrix, plus the abort flag. The LAYOUT is fixed
    even when the matrix is unread, because variable offsets across two
    implementations is a good way to get one subtly wrong; the per-invocation
    COST is what is conditional, so the closest-hit fetches the matrix only
    when the shader reads it. `MaxPayloadSizeInBytes` must move with
    `PAYLOAD_BYTES` in BOTH `lower.py`/`rq_lower.cpp` AND `rq_pipeline.cpp`
    and the harness.

  **Two bugs this work exposed, both worth not repeating.**
  - **The loop isolation check was blind to phi nodes.** It exists to catch
    values escaping the loop, and **a phi is exactly how a value escapes**, so
    it was blind to its own purpose. `Uses()` read call arguments only, and a
    phi is not a call. Both implementations had it identically, which is a
    reminder that byte-identity proves AGREEMENT, not correctness.
  - **A refusal test decayed rather than failed.** `test_reject.py` used opcode
    191 as its "unverified" example; the Unreal survey turned 191 into
    `CandidateTriangleFrontFace`, so the case became a verified one and the
    check started accepting what it was written to refuse. Second instance in
    this project. **A test whose premise is a fact about the world needs
    re-checking when that fact changes.**

- `Abort()` (181): **DONE, coverage 20 of 25.**
  - The brief offered `AcceptHitAndEndSearch()` or a payload flag. Only the
    flag works in general: **a DXR 1.0 any-hit shader has no "reject and
    stop".** Its two terminators are `IgnoreHit()`, which rejects and
    CONTINUES, and `AcceptHitAndEndSearch()`, which accepts and stops. A bare
    `Abort()` needs the missing third.
  - So `Abort` stores a payload flag and the any-hit prologue ignores its
    candidate at once when it is set. Commit-then-abort still accepts, since
    the control flow still falls through; a bare abort still rejects. Either
    way nothing further commits, which is what stopping traversal means for the
    RESULT. Traversal carries on, so this is slower than ideal;
    `AcceptHitAndEndSearch` would be exact for the commit case but only after
    proving the commit dominates the abort in the same iteration.
  - `Abort()` outside the Proceed loop is refused: the any-hit shader is the
    only place traversal can be stopped from.

  **MOST OF Abort CANNOT BE VERIFIED AGAINST WARP, and the passing test does
  not mean it is.** Abort stops at whichever candidate traversal reaches first,
  and that order is implementation-defined, so the committed t, the
  barycentrics and which instance was hit are all legitimately allowed to
  differ between WARP and NVIDIA. A test of those would be meaningless. The
  test therefore observes only WHETHER there was a hit, which is
  order-independent for commit-then-abort: 10377 hits both sides. Pre-setting
  the flag gives 0 hits and DIVERGE, so the flag, its initialisation and the
  prologue check ARE exercised. The TIMING is not, and nothing compared against
  WARP can establish it. **This is the first thing in the project the oracle
  cannot settle.**

- Procedural primitives: **DONE. 7396 hits, bit-exact, on the 1070.**
  `CommitProceduralPrimitiveHit` is 183, `CandidateProceduralPrimitiveNonOpaque`
  190; on the DXR 1.0 side `reportHit` is 158 and an intersection shader is
  kind 8, carrying neither a payload nor an attribute size.

  **The lowering is one body, two substitutions, two shaders.** The intersection
  shader is the SAME Proceed loop body as the any-hit, with one substitution
  changed: an any-hit folds `CandidateType()` to 0, the intersection folds it to
  1, so the triangle branch dies and the procedural branch survives.
  `CommitProceduralPrimitiveHit(t)` becomes `ReportHit(t, 0, attrs)`, and every
  way out of the body becomes a plain `ret void`, because an intersection
  shader has no accept or reject terminator: reporting IS accepting, and
  returning reports nothing. The closest-hit writes status 2, not 1.

  **The obstacle was never in the lowering, and it is now gone.** A query
  committing BOTH triangle and procedural hits lowers, see the both-kinds entry
  below. A SCENE holding both kinds was the other half, and the typed table
  handles that; the two were conflated for a long time and are different
  problems.

- **Nonzero `InstanceContributionToHitGroupIndex`: FIXED.** This used to be a
  limitation that applied TODAY, to triangle-only scenes, and it is now handled
  rather than refused. See the shader table entry below.

- AS interception: **DONE, both halves.** `proxy/as_tracker.{h,cpp}` and
  `proxy/res_tracker.{h,cpp}`, hooked at
  `BuildRaytracingAccelerationStructure`. **The two halves cost wildly
  different amounts, which was the finding, and the expensive one turned out to
  be blocked on an API fact rather than on engineering.**
  - **BLAS geometry types are FREE.** `D3D12_RAYTRACING_GEOMETRY_DESC` arrives
    as CPU MEMORY in the build call, so the type of every bottom-level
    structure is read and remembered with no copy and no sync.
    `ARRAY_OF_POINTERS` is handled as well as `ARRAY`; anything else is left
    unknown rather than guessed at. Verified on the Phase 4 probe, which builds
    one of each, and it distinguishes rather than always saying the same thing.
  - **TLAS instance data was blocked on a missing API, not on cost.**
    `InstanceDescs` is a GPU VIRTUAL ADDRESS, `CopyBufferRegion` takes an
    `ID3D12Resource` and an offset, and **D3D12 has no call that turns an
    address back into a resource.** That was the whole obstacle.
  - The way through: every resource an application creates goes through the
    wrapped device, so `res_tracker` remembers the mapping itself, keyed by
    start address. Two deliberate lifetime decisions. It **holds no reference**,
    because an `AddRef` would change when the application's resources die, which
    a transparent shim must not do, and would leak for every buffer ever
    created; entries can go stale, which is safe because a lookup only happens
    while the application is handing that buffer to a build. And an **entry is
    replaced** when a new resource reports the same start address, which is how
    address reuse after a free is handled.
  - **Two read paths, and only one costs anything.** Upload-heap descriptions
    are mapped and read at record time: no copy, no fence, no stall. That is
    both Microsoft samples and every scene here. GPU-only descriptions get a
    `CopyBufferRegion` recorded into the application's own list, between
    `NON_PIXEL_SHADER_RESOURCE` and `COPY_SOURCE` barriers, and the queue hook
    signals a fence and parses on a LATER submission. **Nothing ever waits**;
    the earlier estimate of a sync per top-level build was pessimistic.
  - Read **once per destination address**, so an engine rebuilding its TLAS
    every frame pays only on the first. The cost of that choice: an application
    that changes its contributions in place keeps the first answer.
  - NOT read: `ARRAY_OF_POINTERS` instance descriptions, where each pointer is
    itself a GPU address needing a second dependent copy. Logged, not guessed.
  - `tier11probe -gpuinst` puts the descriptions in GPU-only memory on purpose,
    so the expensive path is exercised rather than hypothetical. Both paths give
    the same answer with the debug layer on and silent.
  - **What it reads matches the source exactly**: the probe scene comes back as
    2 instances, max contribution 1, reaching triangles and procedural, which is
    what `tier11probe.cpp` sets. Both Microsoft samples come back as 1 instance,
    contribution 0, triangles only, which is the simplest case the table has to
    serve and the one a single record already covered.
  - **The point is the refusal, not the log.** A lowered RayQuery dispatch on a
    scene the table cannot serve now declines and says why, instead of drawing
    it wrong. `raytest --multi --contrib` makes that reachable: WARP finds 18496
    hits and the shim gives none rather than inventing a number.
  - Two limits of that check, both real. It is judged over EVERY top-level
    structure read, not the one the shader is about to trace against, because a
    TLAS arriving through a descriptor table is not identifiable at the
    dispatch, so it can over-refuse; refusing is the safe direction. And it can
    only see what has been READ, so on the GPU-only path the answer is a
    submission late and the first dispatch of a run is not covered.
  - Note: lists are only wrapped on Tier 1.0, so nothing is tracked on WARP.
    Correct, since the shim does nothing there, but it means the tracking
    cannot be observed on the ground-truth path.

- The shader table, built from that data: **DONE.** `proxy/rq_pipeline.cpp`.
  The shim passes zero for `RayContributionToHitGroupIndex` and the geometry
  multiplier, so the record index IS the application's contribution. The table
  now holds **max contribution + 1** records instead of one.
  - **Every record holds the SAME identifier.** The shim has ONE hit group and
    wants it to run for every hit; what the contributions decide is only WHICH
    SLOT a hit lands on, and each slot has to be a valid record for that hit to
    run at all. So the table is sized to the application's layout and filled
    with the shim's single answer.
  - **It grows at DISPATCH, not at pipeline creation.** The acceleration
    structures usually do not exist when the application creates its compute
    pipeline, so how many records the scene needs is not knowable there. It
    starts at one and grows on the first dispatch that needs more. Growth is
    monotonic and replaced buffers are held until the pipeline dies, because a
    dispatch recorded against an old one may still be in flight.
  - Verified with `raytest --multi --contrib`, contributions 0 and 1: **18496
    hits, bit-exact against WARP**, on the scene that was refused before this.
  - **The sensitivity check is the part worth keeping.** With the growth
    disabled the same scene renders 9248 hits, exactly half, with 9248
    hit/miss mismatches: every hit on the instance contributing 1 disappears,
    silently and with no error from anywhere.

- **The mixed case is NOT symmetric, and that was the surprise.**
  `raytest --mixed` builds one triangle instance and one procedural instance
  under one top-level structure. Both directions were measured on it.
  - **Triangle-only shader, procedural geometry present: SAFE.** 14450 hits,
    bit-exact against WARP. The shim's hit group is triangles-only so the
    procedural geometry reports nothing, which is the same answer the shader
    gives on Tier 1.1, where it never commits a procedural candidate either.
    The scene being mixed is irrelevant to it. **This used to be refused.**
  - **Procedural shader, triangle geometry reachable: WRONG.** 15418 hits
    against WARP's 7396. The 8022 difference is exactly the triangle hits: for
    triangle geometry the hit group's intersection shader is not used, the
    closest-hit runs anyway, and it labels everything procedural.
  - So the refusal was conditioned on **what the shader commits**, not on
    whether the scene happens to be mixed. Both now work, see below.
  - **The debug layer cannot settle the safe direction.** It is silent on BOTH
    cases, including the one measured to be wrong, so it does not police hit
    group and geometry type agreement and its silence carries no information.

- Per-index typed records: **DONE, and this was the last structural piece.**
  The table can now carry a record of a DIFFERENT TYPE at each index, so a
  scene holding both geometry kinds is served rather than refused.
  - **Two stub shaders, taken from DXC rather than guessed.**
    `phase5/cases/reference/lib_null_ref.hlsl` asked what a hit group that must
    never commit compiles to. `AnyHitNull` is
    `call void @dx.op.ignoreHit(i32 155)` then `unreachable`, marked
    `noreturn nounwind`; `IsectNull` is a bare `ret void`. Rejecting every
    candidate is how a TRIANGLES group produces no hit, and reporting nothing
    is how a PROCEDURAL one does.
  - **Both measure SFI0 = 0x0**, so neither pulls in a Tier 1.1 feature flag.
    That check is not optional here: `CommittedGeometryIndex` already proved an
    intrinsic can look ordinary and be Tier 1.1.
  - Both are emitted into EVERY lowered library, because which one a scene
    needs is not knowable when the shader is lowered: the acceleration
    structures do not exist yet. Two tiny functions is a cheap price for never
    having to re-lower.
  - **Three hit groups, and the table picks per slot.** The state object
    carries the real hit group plus `HitGroupNullTri` and `HitGroupNullProc`. A
    slot reached by the shader's own kind, by both, or by nothing keeps the
    real record; a slot reached ONLY by the other kind gets the rejecting
    record OF THAT KIND, so the geometry is traversed with a correctly typed
    record and produces no hit.
  - **Result:** a procedural-committing shader on a scene that also holds
    triangles, the two kinds on different records, renders 7396 hits
    **bit-exact against WARP**. That is the case that measured 15418 before.
  - **The sensitivity check.** Replace the rejecting record with the real one
    and change nothing else: 15418 hits again, 8022 mismatches, exactly the old
    wrong answer. The typing carries the whole result.
  - It also makes the SAFE direction well-defined instead of merely observed.
    A triangle-only shader on a mixed scene used to rely on procedural geometry
    meeting a triangles hit group and quietly producing nothing, which could
    only be measured on one driver. Where the contributions are distinct, that
    slot now holds a proper procedural record that reports nothing.
  - **Still refused, and no shim can fix it:** both kinds collapsed onto ONE
    record. That slot would need a procedural record for the procedural
    geometry and a rejecting triangle record for the triangles, and a record is
    one or the other. The application collapsed them.

- A query that commits BOTH kinds: **DONE. The last shader-side gap.**
  `phase5/cases/rayquery_both.hlsl`, one Proceed loop committing a triangle hit
  in one arm and a procedural hit in the other. 9160 hits, bit-exact against
  WARP on the 1070.
  - **One loop body, two shaders**, and the substitution is what separates
    them, exactly as it already did for the single-kind cases. Folding
    `CandidateType()` to procedural kills the triangle arm and the body is an
    intersection shader; folding it to triangle kills the procedural arm and
    the same body is an any-hit. What is new is emitting BOTH from one module.
  - **The dead arm still has to be VALID IR, and that was the actual work.**
    Nothing folds the branch at the text level, so both arms are emitted and
    one never runs. So each shader has to lower the other kind's opcodes: the
    intersection shader drops `CommitNonOpaqueTriangleHit` and turns
    `CandidateTriangleBarycentrics` and `CandidateTriangleFrontFace` into
    constants, because it has no attributes parameter and no `HitKind()`; the
    any-hit drops `CommitProceduralPrimitiveHit` and constant-folds
    `CandidateProceduralPrimitiveNonOpaque`.
  - Those constants are substituted in the same PRE-PASS as `CandidateType`,
    not during emission, so the result does not depend on the order blocks
    happen to be written out in.
  - **Two closest-hits**, because a triangle hit reports committed status 1 and
    a procedural one 2, and a closest-hit cannot say both: it does not know
    which hit group resolved to it. Every other case keeps a single
    `ClosestHit`, so no other output moves by a byte.
  - **Two real hit groups** in the state object, and the typed table puts each
    at the indices its geometry reaches.
  - The test scene is built so the answer does NOT depend on traversal order:
    the procedural commit reports the box front face at z = 0.5, always nearer
    than the triangle at z = 0, so the procedural hit wins wherever both are
    hit. RayQuery traversal order is implementation-defined and WARP is the
    oracle, so a tie would have been untestable.

- **A trap in the .ll production path, worth not rediscovering.** A `.ll` made
  with `dxc -dumpbin | Out-File` is CRLF, and the C++ rewriter rejected it with
  "TraceRayInline does not use the query handle", which is nowhere near the
  truth: `SplitLines` kept the `\r`, the body-end test is a line equality
  against `}`, so the parser never left the function body and the complaint
  named something unrelated. **The Python tolerated it by accident**, because
  its patterns end in `\s*$` and `\r` is whitespace. The suite builds its `.ll`
  with `dxc -Fc`, which writes LF, so nothing covered this. The C++ now drops a
  trailing `\r`.
  - What located it: running the C++ on a **known-good** shader through the
    same path. It failed there too, which put the fault in the path rather than
    in the new code immediately. **When new code fails, run the OLD code
    through the new path before believing the new code is wrong.**

- Dynamic descriptor indexing: **DONE, and it was the last rewriter
  limitation.** Uniform and non-uniform, both bit-exact and both end to end
  through the proxy.
  - **DXC answered the shape, as usual.**
    `phase5/cases/reference/lib_dynarray_ref.hlsl`: the constant case's folded
    getelementptr EXPRESSION simply becomes a real getelementptr INSTRUCTION on
    the same global, with the same load and the same `createHandleForLib` after
    it. `NonUniformResourceIndex` attaches `!dx.nonuniform !N`, the node being
    `!{i32 1}`. No new opcode, no `annotateHandle`, nothing else moves. It was
    smaller than its place on the list suggested.
  - The non-uniform tag is emitted through a placeholder resolved when the
    metadata is rebuilt, and its node is allocated LAST so a shader that indexes
    statically keeps every other node id exactly where it was.
  - **Dropping the non-uniform tag was never an option**: that would be a
    silently wrong lowering rather than a missing feature.
  - The test index is `width / 128`, which is 2 at runtime because `--table`
    binds the real output at slot 2 and decoys at 0, 1 and 3. Writing the 2
    literally would have made this the existing `table` case and proved
    nothing. **Sensitivity: with the index poisoned to a constant 0, the same
    scene renders 0 hits instead of 14450**, because the write lands on a decoy.
  - **A dynamic index inside the Proceed loop IS refused**, precisely. The
    isolation check exempts resource handles from caller state, which is sound
    only when the MODULE fully determines the handle; a dynamic index makes it
    depend on a value computed in the raygen, which the any-hit cannot see.
  - Proving that reachable needed a new kind of check: **refusals that live in
    the LOWERING rather than the analysis had no coverage at all**, including
    the two isolation refusals that predate this work.

- **A harness gap this uncovered.** `RunRayQuery` never honoured `--table`, so
  a shader written against a descriptor table could not run as a RayQuery
  compute shader at all, and the array cases had only ever been tested through
  `--lib`. They had never been through the proxy's dispatch path. Fixed, and
  the `table` case is now in the dispatch suite too, which is coverage that was
  previously IMPOSSIBLE rather than merely absent.

- **WHAT UNREAL ACTUALLY REFUSES, MEASURED (2026-09-22).** With the stream-form
  gap closed, a real UE 5.8.2 run produced ~158 refusals, and they are not
  spread evenly. Counted from one session of Escher:

      105  CandidateGeometryIndex                       (203)
       33  "2 concurrent RayQuery objects"              Lumen x3, Niagara
       12  CandidateInstanceContributionToHitGroupIndex (214)
        4  entry-block phi bug, use of undefined '%bb0'
        3  CommittedGeometryIndex                       (209)
        1  RayFlags                                     (195)

  The three unknown opcodes were identified by asking DXC, not by reading the
  gaps in the table: `phase5/cases/reference/rq_opcodes_ref.hlsl` names the
  whole StateScalar family in one compile and is there so this never has to be
  guessed. 195 RayFlags, 198 RayTMin, 203 CandidateGeometryIndex, 214 and 215
  the two InstanceContributionToHitGroupIndex forms.

- **`GeometryIndex` AND `InstanceContributionToHitGroupIndex`: DONE, all four
  accessors, bit-exact against WARP on the 1070.** 120 of the 158 refusals
  Unreal produced, closed by one mechanism. The reason they were impossible was
  true and is now obsolete: Both were written off
  for reasons that only hold while the APPLICATION owns the shader table:
  `GeometryIndex()` in a DXR 1.0 hit shader is itself Tier 1.1, and HLSL
  exposes no hit-shader intrinsic for the contribution at all. **The shim
  builds the table.** So the record carries the answers as local root signature
  root constants, and the hit shader reads them back.

  With `MultiplierForGeometryContributionToShaderIndex = 1` a hit lands on
  record `InstanceContribution + GeometryIndex`, and the shim knows both numbers
  for every record it writes.

  That is one mechanism for FOUR accessors, 120 of the 158 refusals above.

  **MEASURED, and this was the gate: SFI0 = 0x0.**
  `phase5/cases/reference/lib_localroot_ref.hlsl` reads both values from
  `cbuffer ... : register(b0, space1)` in an any-hit and a closest-hit, and the
  container carries NO feature flag at all. That is the whole difference from
  `GeometryIndex()`, which sets 0x2000000 and makes `CreateStateObject` return
  E_INVALIDARG on the 1070. Not optional to check: this project has already
  been caught once by an intrinsic that looked ordinary and was Tier 1.1.

  The shape is machinery the rewriter already has: a global of a named type,
  `createHandleForLib` (160) on it, then `cbufferLoadLegacy` (59). The same
  synthesis it already does for `@rq_uav0`.

  **SUPERSEDED IN 0.37.0: the local root signature below is gone.** A hit
  shader reading it crashes the Pascal driver, so the values are baked into a
  copy of the hit shaders per pair instead. The per-record pairs, the table
  sizing and the collision refusal still hold; the delivery changed. See the
  driver crash entry.

  How it is built: per-record (geometryIndex, instanceContribution) from the
  instance data, with the table sized by `max(contribution + geometryCount)` and
  a REFUSAL when two different pairs collide on one record, because one record
  answers once; a local root signature of two root constants at b0 space1,
  associated with the HIT GROUPS only so raygen and miss records stay 32 bytes;
  hit records growing to 64; the candidate forms read in the any-hit and
  intersection shaders, the committed forms carried in the payload from the
  closest-hit. The payload went 88 to 92 bytes and `PAYLOAD_BYTES` moves
  together in `lower.py`, `rq_lower.cpp`, `rq_pipeline.cpp` and the harness, or
  `CreateStateObject` fails.

  **The test had to exist before the feature could be believed.** Every scene
  here had ONE geometry per structure, so every correct answer was 0 and a
  lowering that dropped the index would have passed. `--geom` builds one BLAS
  with four geometries, and the candidate values GATE THE COMMIT rather than
  escaping the loop, which the rewriter refuses and rightly. `geometry +
  contribution != 3` commits, so the missing quadrant is geometry 3 without
  `--contrib` and geometry 1 with it: **the gap MOVES**, so neither accessor
  can be a constant.

  Three bugs caught by running it, none by reading it: the
  `needsRecordConstants` flag was set on one collection branch of two and so
  missed every candidate form; the payload field list stopped at 9 so the
  raygen read `%rq.pl10` with nothing defining it; and `test_reject.py` asserted
  CommittedGeometryIndex must be REFUSED, which failed the moment it started
  working. That last one is the THIRD time here a test encoded a fact about the
  world and the fact moved. It is kept and flipped to an acceptance.

- **`RayFlags` (195): DONE, and it exposed a silent wrongness.** 59 of Unreal's
  shaders refused on it once GeometryIndex stopped blocking them first. Two
  routes: `dx.op.rayFlags` (144) inside the loop, where it is legal, and a
  FOLDED CONSTANT in the raygen, where it is not. A test reading it in one place
  only would not tell them apart.
  - The lowering had been trusting the TraceRayInline flags to be a
    compile-time constant. `_imm` gives None otherwise and `(dyn_flags or 0)`
    turned that into 0, so a shader computing flags at runtime would have
    traced with the WRONG flags, silently. Now refused, in both implementations.
  - The first version of the test used CULL_BACK_FACING_TRIANGLES and culled
    the only triangle, so both sides reported 0 hits and it could not fail.

- **The ENTRY-BLOCK phi bug: FIXED.** The oldest known defect here, and the
  last of the assembler failures. DXC gives the entry block no label, because
  nothing can branch to it, so normalisation had nothing to rename; but an `if`
  with no `else` straight out of the entry makes a phi NAME it as a
  predecessor, and `%bb0` then referred to a block that did not exist.
  - The normaliser adds the label only when the function references it, so
    modules that never needed one stay byte-for-byte unchanged.
  - Both module parsers always created an implicit entry block, so a labelled
    first block became a SECOND block and the entry came out empty. A labelled
    first block is now taken as the entry.
  - **It survived the whole of Phase 5 because EVERY case opened with
    `if (tid.x >= width) return;`.** That guard puts a block between the entry
    and everything else. One shared habit across every test, hiding one bug.
    Same shape as the assumption the independent shader found: a suite made of
    cases written to demonstrate a lowering shares its author's blind spots.

- **Two bugs only a real engine's CONTROL FLOW exposes, both fixed.**
  - **A phi's predecessor blocks were counted as value reads.** A phi writes
    them as `[ %val, %bb12 ]`, with no `label` keyword, so the operand scan saw
    %bb12 as a value and the isolation check refused 118 Unreal shaders for
    "reading" their own blocks. `Uses()` already decodes a phi correctly, so a
    phi now delegates to it.
  - **The metadata scan did not match `distinct`.** A [branch] or [loop] hint
    makes DXC emit `!16 = distinct !{!16, "dx.controlflow.hints", i32 1}`. That
    id stayed invisible, `max(md)+1` started too low, and the fresh-id counter
    handed out an id already in use: "Metadata id is already used". Unreal uses
    those hints constantly.
  - `rayquery_phi.hlsl` carries both. `[branch]` is load-bearing in it: without
    it DXC flattens the if into a select, there is no phi, and the case tests
    nothing.

- **CLOSING ONE REFUSAL EXPOSES THE NEXT, and that has now happened four
  versions running.** GeometryIndex hid RayFlags and loop isolation; those hid
  the runtime ray flags; that hid the phi bug; the phi bug hid the `distinct`
  metadata bug. Each was invisible while the one in front of it refused the
  same shaders first. Expect the count to stay high and the COMPOSITION to be
  the signal, not the total.

- **Ray flags computed at RUNTIME: supported.** `dx.op.traceRay` takes
  RayFlags as an ordinary i32 operand, not an immediate. Measured, see
  phase5/cases/reference/lib_dynflags_ref.hlsl. Unreal does this in 118
  shaders, which became the largest refusal the moment the others were closed.
  - The lowering used to FOLD the flags, and `(dyn_flags or 0)` silently turned
    an unknown value into 0, so a shader traced with the wrong flags and
    nothing said so. That was refused for one version, correctly; passing the
    operand through is the actual answer. The refusal was right about the bug
    and wrong about the question.
  - `Query.ray_flags` still reports only the STATICALLY KNOWN bits, and that is
    deliberate: it is what decides whether traversal is provably fixed-function.
    A query with no Proceed loop and no FORCE_OPAQUE template is still refused,
    because a runtime value cannot prove it.

- **Loop isolation: WIDENED, correctly, and the primitive under it was
  wrong.** Unreal refused 59 shaders on "reads values defined outside it",
  which is the ordinary shape of an alpha test: read the thresholds once,
  compare per candidate.
  - **A cbuffer read, the ray index and pure arithmetic on them are NOT caller
    state.** The cbuffer is bound by the global root signature and holds the
    same bytes for the whole dispatch; DispatchRaysIndex in the any-hit is the
    SAME ray. So the generated hit shader rebuilds the chain instead of
    refusing. Same reasoning that exempted resource handles, one level up.
  - **What is NOT on the whitelist is the point.** No loads, because a UAV the
    raygen wrote reads back differently. No phis, because a phi depends on
    which path the RAYGEN took, which is the caller state the payload cannot
    carry. No integer division, because recomputing hoists it and division by
    zero is undefined. No other dx.op, the same rule the opcode whitelist has.
  - **`Instr.uses()` only decoded CALL ARGUMENTS and phi incomings.** A value
    reaching the loop body through ordinary arithmetic, `fcmp float %bary,
    %thresh`, was invisible to the isolation check. The generated shader then
    referenced a value it never defined and the ASSEMBLER said so: loud, but
    nowhere near the cause. Almost certainly what the 6 "use of undefined
    value" failures in Unreal's log were. There is now a complete operand scan.
  - **Types are told from values by asking the module**, not by guessing:
    a name is a type exactly when the module declares `%name = type`.
    `%rq.pl` and `%dx.types.Handle` look alike.
  - **The closure must skip what the body defines for itself**, or it walks
    back into the loop and rebuilds those instructions in the prologue too,
    which is a duplicate definition.
  - **The boundary is tested, not just the feature.** `param` proves the
    exemption works, `refuse_uav_in_loop` proves it STOPS. An exemption is
    exactly the kind of thing that widens quietly.

- **UNREAL HAS NO EMULATION CODE FOR PASCAL, and "emulated" describes the
  DRIVER, not the engine.** Asked because if Epic shipped an emulator it would
  save this project enormous work. It does not exist.
  - Grepped all of `Engine/Source/Runtime` for `emulat` in the RHI layers.
    Every hit is the device id list, shader BUNDLE emulation, or Intel's
    emulated atomic64. `IsRayTracingEmulated` has ONE caller and does ONE
    thing: `DisableRayTracingSupport()`. There is no implementation behind it.
  - `Pascal` appears nowhere in the runtime except units of pressure in
    UnitConversion.cpp.
  - Unreal drives the DXR 1.0 API identically for every card.
  - Lumen's "software ray tracing" is SIGNED DISTANCE FIELD tracing
    (UseMeshSDFTracing, UseGlobalSDFTracing, heightfields). A different
    technique: no BVH, no triangles, never touches an acceleration structure
    or a RayQuery shader. Not a fallback we could borrow.
  - So the name means what this project's premise already said: NVIDIA's DRIVER
    runs BVH traversal and ray/triangle intersection on Pascal's shader cores
    instead of RT cores. The emulation is below the API.
  - **`GAllowEmulatedRayTracing = 1` therefore enabled nothing. It disabled a
    REFUSAL.** Which makes the dxgi device id spoof functionally identical to
    that old cvar: it defeats a model-name check, not a capability check, which
    is exactly why it changes only the device id and leaves every capability
    answer truthful.
  - INFERRED, not verified: that Epic's motive was performance. The only
    action is to disable and no comment states a reason. If so they were not
    wrong, and a working Escher should be expected to be SLOW. That is the
    driver, not the shim, and the brief already accepts 1 fps.

- **UNREAL'S SHADERS ARE SHADER MODEL 6.6, AND THE REWRITER WAS BUILT FOR
  6.5.** The single biggest finding since the device id denylist, and it
  explains 124 of the 157 refusals at a stroke. Measured from the dumped
  containers, not inferred.

      cs_6_6 (Unreal)  createHandleFromBinding (217) -> annotateHandle (216)
      lib_6_6 (target) createHandleForLib      (160) -> annotateHandle (216)

  `refused_010.dxil` is `!{!"cs", i32 6, i32 6}` with SEVEN uses of 217 and
  ZERO of either 57 or 160. Every resource chain looks like

      createHandleFromBinding -> annotateHandle -> cbufferLoadLegacy ->
      extractvalue -> icmp

  which is EXACTLY the recomputable shape already supported, written in the
  newer opcodes. So:
  - the 118 "loop body reads values defined outside it" are handles made with
    217, which is not on the recomputable list, so nothing is exempt;
  - the 6 "Internal declaration 'rq_cbv0' is unused" are the globals the
    rewriter synthesises for every resource, which a 6.6 module never loads.

  **The target shape, read off DXC** (phase5/cases/reference/lib_sm66_binding_ref.hlsl):

      %CB = type { float, i32 }
      @CB = external constant %dx.types.Handle        ; HANDLE type, not %CB
      !6 = !{i32 0, %CB* bitcast (%dx.types.Handle* @CB to %CB*), !"CB", ...}

      %1 = load %dx.types.Handle, %dx.types.Handle* @CB, align 4
      %2 = call %dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %dx.types.Handle %1)
      %3 = call %dx.types.Handle @dx.op.annotateHandle(i32 216, %dx.types.Handle %2, %dx.types.ResourceProperties { i32 13, i32 8 })

  Differences from the 6.5 path the rewriter implements: the global is
  `%dx.types.Handle` rather than the resource type, the overload is
  `.dx.types.Handle` rather than `.CB`, the record BITCASTS the handle global,
  and an annotateHandle follows and must be kept.

- **Shader Model 6.6 binding: DONE (0.25.0), and a real Unreal shader now
  lowers, validates and signs.** `RayTracingDebugMainCS`, dumped from the
  running game, goes container in and signed container out through
  `dxrw rewrite`, and the Python and the C++ produce byte-identical text.
  217 becomes `createHandleForLib` (160), the same target 6.5 already used, and
  216/217 join the recomputable list so the handles stop looking like caller
  state. The target shape above is implemented exactly as measured.
  - **The module shader flags were hardcoded and happened to be right.** The
    lowering wrote `i64 16`. Every shader this project had seen declared
    `0x2000010`, and the only bit the lowering removes is the tier 1.1 flag
    `0x2000000`, so 16 was correct by coincidence. Unreal declares
    `0x42000010`, and the validator said "Flags must match usage. Flags
    declared=16, actual=1073741840", which names the symptom and not the
    cause. The flags now come from the entry point's properties with that bit
    cleared, which reproduces 16 exactly for every existing case, so nothing
    else moved by a byte.
  - **`createHandleFromHeap` (218) turns out to be sound as it stands.** A heap
    handle used only in the raygen stays where it is, unchanged and correct.
    One used inside the Proceed loop is refused, because 218 is NOT on the
    recomputable list and the isolation check therefore catches it. The
    blanket refusal the docs promised would have blocked the raygen case for
    nothing. Measured on `RayTracingDebugMainCS`, which uses 16 of them and
    keeps all 16 in the raygen.
  - **The independent case earned its keep again, on its first run.**
    `phase5/cases/rayquery_sm66.hlsl` is the `indep` shader at 6.6, so it
    converts a handle in the raygen AND recreates one in the generated any-hit.
    It failed in a way the Unreal shader never could: LLVM prints an all-zero
    `ResBind` as `zeroinitializer` rather than writing the fields out, and
    nothing Unreal bound sat at SRV t0 space0. `build_phase5.bat` compiles
    every `*sm66.hlsl` at `cs_6_6` after the 6.5 pass, so the shader model is
    the test and the HLSL is deliberately not 6.6-specific.
  - **Still refused, and named honestly:** a resource array reached through a
    6.6 binding and indexed dynamically. The 6.5 path supports exactly that,
    through a getelementptr on the array global. The 6.6 form is refused only
    because the shape of an ARRAY global in the binding form has not been
    measured off DXC. One reference compile would settle it.
  - Two C++ divergences from the Python surfaced here, both invisible until a
    shader needed record constants AND ray flags at once: `@rq_record`
    declared after the resource globals rather than before, and the closest-hit
    reading the record before the index fields rather than after. Neither
    changed behaviour. **That is what byte-identity is for: it catches drift
    while it is still cosmetic.**
- **HARDCODED ATTRIBUTE GROUP NUMBERS: FIXED (0.26.0), and the message pointed
  nowhere near the cause.** Three of the six shaders a real Unreal session
  refused were this one bug.

      ours     #0 readnone  #1 nounwind           #2 nounwind readonly  #3 added
      Unreal   #0 readnone  #1 nounwind readonly  #2 nounwind           #3 ABSENT

  `#3` was appended by replacing the literal line
  `attributes #2 = { nounwind readonly }`, which that module does not contain,
  so it never appeared. `AnyHitNull` was marked `#3`, which resolved to
  nothing, so `IgnoreHit` was not noreturn, so the `unreachable` after it was
  rejected: "Instructions must be of an allowed type". **The validator names
  the instruction, not the attribute that made it illegal.**
  - Generated text now writes placeholders resolved against the input module,
    the same technique already used for the non-uniform metadata node. Ours
    still resolve to 1, 2 and 3, so every existing output is byte-identical.
  - **`#0` was hardcoded too and happened to be right**, readnone in both. It
    is resolved now as well rather than left to keep being lucky.
  - **`dx.op.dispatchRaysIndex` was declared unconditionally**, and an unused
    declare is itself a validation error. It only replaces `threadId`, and
    EVERY shader in this suite reads `SV_DispatchThreadID`, which is exactly
    why it was unconditional and why nothing here could find it. Same shape as
    the entry-block phi bug, which every shader hid by opening with a bounds
    check.
  - **The test could not have existed before the bug did.** Every input in the
    suite agrees with the hardcoded numbering, so no case could expose it.
    `test_reject.py` now rewrites a known-good module to the other numbering
    before lowering and checks each generated function carries the id its
    module actually uses. Put the bug back and the check fails.

- **A REAL UNREAL SESSION ENDED IN A GPU CRASH, NOT A COMPILE FAILURE, AND IT
  IS NOT EXPLAINED.** `CrashType GPUCrash`, `D3DDeviceRemovedReason` =
  `0x887A0020` = `DXGI_ERROR_DRIVER_INTERNAL_ERROR`, read from Unreal's own
  crash context. Not a hang, not a page fault. The shim saw it as
  `CreateStateObject failed (hr=0x887A0005)`, which is the symptom.
  - **Four Unreal shaders had lowered and built state objects successfully
    first.** That is the first time real engine shaders got that far.
  - Two candidates and nothing yet separating them: the REFUSED shaders, which
    are forwarded unchanged so a RayQuery DXIL reaches a Tier 1.0 driver, or
    the libraries this shim GENERATED. 0.26.0 reduces the first without
    establishing a cause.
  - **The next move is a diagnostic, not a guess.** `dump` writes refusals
    only, so the four that lowered are gone and cannot be replayed. Dumping
    lowered output too puts them back on disk, where `CreateStateObject` can
    be tried offline on the 1070 without the game.
  - DRED was off in that run (`RHI.DRED false`), so there are no breadcrumbs.
    Aftermath was on, which may be worth reading next time.
  - The device id spoof is confirmed working from the engine's side:
    `RHI.DeviceId 1F08`.

- **THE GPU CRASH IS STILL UNEXPLAINED AFTER FIVE VERSIONS, AND THE CONTROL WAS
  NEVER RUN.** 0.26.0 through 0.31.0 each fixed a real defect found while
  chasing it, and none of them was the cause. What IS established, by
  measurement rather than argument:
  - **The generated libraries are fine.** All seven dumped from a crashing run
    build on the 1070 one at a time, and all seven build and are HELD at once
    in one process with the device alive afterwards. All seven build on WARP
    too, so they are valid DXR 1.0 rather than merely tolerated.
    `phase5out\sotest.exe --dir <folder> hw` does it in about a second.
  - **No lowered dispatch has ever executed.** The `rqdispatch` line never
    appeared in the log, and that setting is read inside the dispatch path.
  - **No acceleration structure build was ever intercepted**, and no
    `ExecuteIndirect` on the DISPATCH_RAYS stand-in. So the AS tracking and the
    indirect split, the two most complex things here, never ran either.
  - `SecondsSinceStart = 0`: it dies during startup, before anything renders.
  - The failure is `CrashType GPUCrash`, `D3DDeviceRemovedReason 0x887A0020`,
    `DXGI_ERROR_DRIVER_INTERNAL_ERROR`. Not a hang, not a page fault.

  **The mistake in method, worth more than any of the fixes**: every experiment
  so far varied something INSIDE the translation and asked whether the crash
  moved. None asked whether the translation is involved at all. `nowrap = 1`
  is documented in this brief as precisely that diagnostic, "if the symptom
  survives nowrap = 1, the translation is not the cause", and it went unrun for
  five versions while four bugs were found and fixed around it.

  `rqlimit = 0` looked like that control and was not: Unreal turns the first
  forwarded shader into a fatal error, so the run ended long before the point
  where the driver had been dying.

  **The live hypothesis it tests.** The dxgi spoof stops Unreal refusing Pascal
  by device id, so Unreal enables ray tracing and drives DXR 1.0 paths on a GTX
  1070. Epic disabled that for a reason nobody here has established. If the
  driver cannot survive what Unreal asks of it, no amount of correct RayQuery
  lowering will help, and that is a different project from this one.

- **READING THE ENGINE SOURCE, AFTER A DAY OF GUESSING.** `C:\DW\UnrealEngine`
  is a full UE source tree and it sat there through eight versions of chasing
  the GPU crash by inspection. Two things came straight out of it:
  - **`-gpucrashdebugging` turns DRED on.**
    `UE::RHI::ShouldEnableGPUCrashFeature` in `RHI/Private/RHI.cpp` makes that
    one switch force every GPU crash feature on, over any cvar. **Every crash
    report collected so far says `RHI.DRED false` and
    `RHI.DREDHasBreadcrumbData false`**, which is why eight runs produced "the
    GPU died" and not one of them said what it was doing. The engine has had
    the answer available the whole time.
  - **Compute PSOs are created on WORKER THREADS.**
    `FD3D12PipelineState::CreateAsync` starts an
    `FAsyncTask<FD3D12PipelineStateWorker>`, so `TryCreate` runs concurrently
    on several threads. Checked in consequence: the DXC host creates its
    `IDxcUtils`, `IDxcCompiler`, `IDxcAssembler` and `IDxcValidator` per call
    rather than sharing them, and its one-time load is behind a
    `std::once_flag`, so that path is sound. The carrier registry has its own
    mutex.

  **What the offline probe has ruled out**, so none of it is worth re-testing:
  every generated library builds on the 1070 individually, all seven build and
  are held at once, and **210 held at once leaves the device alive**. It is not
  one bad library, not cumulative creation, not a resource limit.

  **What the bisect established**: `rqstub = 1` runs the game with ray tracing
  enabled and Unreal's own DXR work intact, so the cause is on this side.
  `nowrap = 1` runs it too but proves less, because Unreal switches ray tracing
  off entirely at Tier 1.0.

  **The lesson, and it is the same one as the entry-block phi and the stream
  form**: the answer was in something already on this machine, and it went
  unread because guessing felt faster. Eight versions, four real defects, none
  of them the cause.
- **ESCHER'S RAY TRACING WORKS ON PASCAL THROUGH PLAIN DXR 1.0, AND THAT WAS
  FOUND BY ACCIDENT.** A run where `d3d12.dll` STOOD ASIDE, because
  `dxcompiler.dll` had gone missing from the game folder, left only the dxgi
  device id spoof in play. Unreal therefore saw a Turing id and the REAL Tier
  1.0, enabled ray tracing, drove its own DXR 1.0 paths, and the game rendered
  with **correct colours and lighting** and exited cleanly.
  - So Epic's `IsRayTracingEmulated` refusal really is the only thing standing
    between this card and a working ray-traced frame in this game, exactly as
    the device id work assumed, and the driver survives everything Unreal's
    DXR 1.0 path asks of it for a whole run.
  - It also settles what the brightness and hue shift always were: the
    do-nothing substitution, as designed, and nothing else. Every earlier run
    that looked broken was broken on purpose.
  - And it narrows the remaining problem to RayQuery alone.

- **A RUN THAT LOOKED LIKE A TRIUMPH WAS THE SHIM DOING NOTHING, IN THE
  FIELD.** The same run as above. The picture was right for the first time all
  day, and the log's fourth line said:

      DXC: NOT AVAILABLE, dxcompiler.dll is not next to the shim
      standing aside entirely ... the application gets the real device untouched

  This brief ALREADY warns about exactly this, for `run_proxy_test.ps1`: "the
  Microsoft sample folders contain no DXC, so the shim stood aside there and
  the comparison became the unmodified path against itself. It would have
  passed forever while proving nothing." The warning was written for a test
  script and the same thing then happened to a real run, where the reward for
  believing it would have been declaring the RayQuery translation fixed.
  - **Read the DXC line before interpreting any run**, alongside the rule
    about reading the configuration line. Both are one line and both decide
    whether the run means anything.
  - The two DLLs had simply been moved out of the folder by hand and not put
    back. That is the point rather than a footnote: the shim can end up
    standing aside for entirely ordinary reasons, so the DXC line has to be
    read on every run and not only when something looks wrong.
  - Standing aside is still the right behaviour. The failure was in reading
    the result, not in the shim.

- **THE UAV-APPEND REFUSAL IS NOT CHEAP, AND THE GUESS THAT IT WAS DEBUG-ONLY
  WAS WRONG.** Named from the dump rather than assumed:

      4  VolumeHardwareRayTraceLightSamplesCS          MegaLights
      4  HardwareRayTraceLightSamplesCS                MegaLights
      1  LumenSceneDirectLightingHardwareRayTracingCS  Lumen
      1  RayTracingDebugMainCS                         the debug pass

  Nine of the ten are real rendering. Refusing them costs MegaLights and Lumen
  hardware ray tracing, which is most of what this game's ray tracing does.

  **They share ONE construct, not nine algorithms.** Disassembled side by
  side, the Lumen one and the debug one are instruction for instruction the
  same shape: a counter increment on a `RWStructuredBuffer<stride=256,
  counter>`, a record whose first field is the constant **94**, `RayFlags`
  stored at offset 76, all behind a runtime flag read from a cbuffer. That is
  one piece of instrumentation appearing in unrelated passes.

  **It comes from the engine's SHARED wrapper**, not from the shaders.
  `Engine/Shaders/Private/RayTracing/TraceRayInline.ush` owns the
  `while (RayQ.Proceed())` loop and calls back into each shader through
  `Callback.OnAnyHit(...)`. So every hardware-ray-tracing pass in Unreal
  inherits the same loop body shape, which is why one refusal takes out a
  whole family.

  **THE REFUSAL MESSAGE OVERSTATES ITS OWN CASE.** "Runs a different number of
  times" is not right in general:
  - with `FORCE_OPAQUE`, `Proceed()` yields nothing on Tier 1.1 either, so both
    do ZERO appends;
  - otherwise the any-hit runs once per non-opaque candidate, which is the
    same set `Proceed()` yields, so the COUNT matches.

  What genuinely differs is `Abort()`, where this shim's lowering lets
  traversal continue and so would append extra records, and ORDER, which is
  implementation-defined for RayQuery traversal anyway and therefore already
  not guaranteed for a counter-append buffer.

  **THE SPEC WAS CHECKED AND IT SAYS THE REFUSAL IS RIGHT AS WRITTEN, TODAY.**
  From the D3D12 raytracing spec, on
  `D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION`:

      "With this flag, the any hit shader must only execute once for a given
       intersection on a given ray."

  The flag existing is the proof: **without it an implementation MAY invoke the
  any-hit more than once for the same intersection**, and the spec separately
  says "there is no defined order of execution of any hit shaders for the
  intersections along a ray path", and warns apps to "be careful about
  authoring side effects... such as doing UAV writes from them". So an append
  transplanted into an any-hit can fire twice for one candidate. `Proceed()`
  yields each candidate once. The two are not equivalent and the refusal
  stands.

  **BUT THE FLAG IS ALSO THE WAY THROUGH, AND THE SHIM IS IN A POSITION TO SET
  IT.** Geometry flags live in `D3D12_RAYTRACING_GEOMETRY_DESC`, which arrives
  as CPU MEMORY in `BuildRaytracingAccelerationStructure`, which this shim
  already intercepts and already reads for free. Adding the flag means copying
  the array and pointing the build at the copy.

  The argument for doing it is not about appends, it is about faithfulness:
  **RayQuery semantics already imply the flag.** `Proceed()` yields each
  candidate exactly once, so any lowering that turns a Proceed loop into an
  any-hit shader is only correct when that shader runs exactly once per
  candidate. Today that happens to be harmless because every loop body the
  rewriter accepts is idempotent. It stops being harmless the moment one is
  not, which is precisely this case.

  Costs and open questions, none of them settled:
  - the flag applies to the application's own DXR 1.0 hit groups too, where it
    forces de-duplication that was not asked for. Slower, not wrong;
  - it changes an acceleration structure the application built, which no other
    part of this shim does;
  - ORDER is still undefined even with the flag, so a write whose result
    depends on the order of candidates is still not transplantable. A counter
    append is order-independent by construction, which is why this family
    would be served;
  - `Abort()` still differs, because this shim's lowering lets traversal
    continue, so it would append records RayQuery would not. That one stays a
    refusal.

  So the narrowed rule, if this is built: set the flag, then refuse a side
  effect only when the query uses `Abort()` or the store is not indexed by the
  counter result.

- **THE DRIVER CRASH REPRODUCES OFFLINE. ONE SECOND, NO GAME.**
  `phase5/cases/driver-crash/` holds it. `crash.out.dxil` is a library this
  project generated from `VolumeHardwareRayTraceLightSamplesCS`, a real UE
  5.8.2 compute shader. Built with its own root signature on the 1070, on a
  fresh device, in a process that does nothing else:

      phase5out\sotest.exe crash.out.dxil crash.rs.bin hw
      calling CreateStateObject with 9 subobjects...
      Segmentation fault

  The driver access-violates INSIDE `CreateStateObject`. No Unreal, no other
  state objects, no memory pressure, no concurrency. This is what eleven
  versions of bisecting could not reach.

  **IT IS INTERMITTENT AND IT WEARS OFF, WHICH IS THE MOST IMPORTANT PROPERTY
  IT HAS.** The first version of this entry named `lowered_008` and said it
  crashed reliably. It does not. Measured across nineteen generated libraries
  from one run, each built alone in its own process:

      first time each was ever compiled    5 of 19 crashed
      sweep again                          1 of 19, the same one
      sweep again                          1 of 19
      sweep again                          none
      sweep again                          none

  The library that crashed twice then ran ten times without complaint. **So one
  clean run proves nothing, and a bisect that trusts one is worthless.** Take
  the result over several fresh sweeps. The vendored case is now
  `HardwareRayTraceLightSamplesCS`, which survived longest, and the README says
  this in its first paragraph.

  **THE CACHE TEST WAS RUN AND IT CONFIRMS THAT.** NVIDIA's DXCache under
  %LOCALAPPDATA%, 550 MB, emptied, then the same nineteen swept three times:

      cold cache    6 crashed   008 009 010 012 013 015
      again         2 crashed   010 012
      again         1 crashed   010

  The same decay as before, from a known starting point. So the crash happens
  while the driver actually COMPILES the library, and a compile that survives
  leaves a cache entry that stops it happening again.

  **It is PROBABILISTIC PER COMPILE, not a property of a given library.** The
  cold-cache set is not the set from the first sweep of all: 016 and 018
  crashed then and survived the cold run, while 009 and 012 did the reverse,
  and 010 needed three attempts to get through. A driver shader compiler that
  fails about a third of the time on the same input is a race or uninitialised
  memory, not a rejected construct.

  **THE "ONE SHADER FAMILY" READING BELOW IS REFUTED, and it is kept only
  because the way it failed is instructive: every number in it is n = 10 or
  less on a failure that is intermittent, so the split it reports is noise.
  At 20 trials 015 goes from 0 of 10 to 13 of 20, and 011 and 014, called
  safe here, are 6 and 8 of 10. The real sort is `recordconstants`, see the
  finding below.**

  **AND THE CRASHERS ARE ONE SHADER FAMILY.** All six are MegaLights light
  sampling:

      CRASH  008 009 010     VolumeHardwareRayTraceLightSamplesCS
      CRASH  012 013 015     HardwareRayTraceLightSamplesCS
      ok     011 014 016 017 the rest of that same family
      ok     000 001         LumenReflectionHardwareRayTracingCS, the two
                             BIGGEST at 48144 and 53852 bytes
      ok     002..007 018    Niagara, Barycentrics, DebugPicking,
                             DebugTraversal, LumenSceneDirectLighting

  Six of the ten in that family, none of the nine outside it on that sweep.
  That is the same family that produces the UAV-append refusal, and the same
  family that is the largest refusal source overall. Whatever is special about
  these shaders is worth finding once.

  **WHAT THE CRASHING FAMILY CONTAINS THAT THE SAFE ONES DO NOT.** The
  generated libraries were disassembled and their dx.op use compared. Four
  things separate the MegaLights family from every library that has never
  crashed, including the two Lumen reflection ones that are nearly twice the
  size:

      op                    Lumen 000/001   MegaLights 008..016   LumenDL 018
      Sin                        0                6                   0
      Cos                        0                6                   0
      LegacyF16ToF32             0                5 to 6              0
      LegacyF32ToF16             0                1 to 2              0
      Sqrt                       4 to 6          26 to 28             3
      Rsqrt                      5 to 6          28 to 30             2

  Sin, Cos and the half-float conversions are ABSENT from everything that has
  never crashed and present in everything that has. Sqrt and Rsqrt are five
  times denser. Going the other way, `Dot4` and `WorldToObject` appear only in
  the safe Lumen pair, so this is not simply "more of everything".

  Note `LegacyF32ToF16` is the packing conversion, NOT a native 16-bit op, so
  it is not the thing the debug layer complains about in Unreal's own compute
  shaders. Pascal supports it. It is a correlation, not an accusation.

  CAVEAT, and it is the reason this is a lead and not a finding: 016 carries
  the same signature and survived the cold sweep, having crashed in an earlier
  one. With a failure that is probabilistic per compile, family-level
  correlation is exactly what a per-compile race would look like too, so this
  does not yet distinguish "the driver cannot compile this construct" from
  "these libraries take a path where the race is reachable".

  **THE CAUSE IS FOUND, AND IT IS OURS: A HIT SHADER READING THE LOCAL ROOT
  SIGNATURE CONSTANTS IS WHAT KILLS THE DRIVER.** Measured on the 1070, the
  NVIDIA shader cache cleared before every single trial:

      base           lowered_010, untouched                 21 of 29 SEGFAULT
      ahonly         record read in the any-hit only         6 of 15
      chonly         record read in the closest-hit only     7 of 15
      globalrec      SAME code, values from a GLOBAL cbuffer 0 of 15
      norecval       record reads removed                    0 of 10
      norecread      reads removed, local root sig STILL     0 of 10
                     built and associated
      n013           the same edit on lowered_013            0 of 15
      lowered_013    that library untouched                  8 of 15

  `globalrec` is the control that carries the result. It keeps the downstream
  code byte for byte, keeps the values dynamic, and changes ONE thing: the
  cbuffer is the global one at cb0 space0 instead of the record one at cb0
  space1. It never crashed. So it is not the arithmetic, not the size and not
  the `rawBufferLoad` those values index.

  `norecread` is the other half: the local root signature subobject is still
  created and still associated with the hit groups, and only the READ is gone.
  Also clean. **Creating the local root signature is fine. Reading from it in
  a hit shader is what the driver cannot compile.**

  **And it sorts all nineteen libraries perfectly.** Sweeping every one with
  the cache cleared each trial, the four with `recordconstants=0` are
  **0 of 80 combined** (003, 004, 006, 007, 20 trials each), and every one of
  the fifteen with `recordconstants=1` crashes given enough trials.

  So this is not a Pascal defect we merely tripped over. **It is the mechanism
  this project introduced in order to close 120 of Unreal's 158 refusals**:
  the shim delivers `GeometryIndex` and `InstanceContributionToHitGroupIndex`
  as local root signature root constants in the shader record, and reading
  them is what removes the device. The feature that unlocked the most and the
  bug that blocks everything are the same line of work.

  **A ROOT CBV DESCRIPTOR DOES NOT HELP, AND NEITHER DOES THE REGISTER
  SPACE.** Both were the cheap escapes and both are dead. `sotest` now takes
  `SOTEST_LOCAL_CBV=1`, which makes the local root signature carry a root CBV
  DESCRIPTOR instead of root constants, and `SOTEST_LOCAL_SPACE=N`, which moves
  the register space. The library is untouched by either: it still reads a
  cbuffer at b0, so only the binding moves, which is what makes each a
  one-variable test.

      root constants, space 1    var_base 11/20, lowered_013 9/20, 015 9/20
      root CBV,       space 1    var_base 13/20, lowered_013 11/20, 015 10/20
      root constants, space 2    var_base 15/20
      no local read at all       0 of 50, see the table above

  So it is not the parameter TYPE and not the register SPACE. **It is reading a
  cbuffer bound through the LOCAL ROOT SIGNATURE at all, from a hit shader.**
  Whatever backs the binding, the driver takes the same path and dies on it.

  **THE FIX SHAPE IS MEASURED AND IT WORKS.** Before building it into both
  rewriters, the two risks were tested on a stub: whether a library carrying a
  hit group per record compiles at all on this driver, and whether its SIZE
  brings the crash back by another route. Neither does. `sotest` takes
  `SOTEST_HITGROUPS=N`, which builds N hit groups from `AnyHit_i` /
  `ClosestHit_i` and skips the local root signature entirely, and
  `phase5/cases/driver-crash/` carries the generated libraries.

      N=0   record reads removed, one hit group   1525 ms cold   0 of 10
      N=27  27 baked hit groups,  61856 bytes     2285 ms cold   0 of 20
      N=64  64 baked hit groups, 107388 bytes     3730 ms cold   0 of 15

  27 is what a real Escher scene asked for. 64 is well past it, 34 and 71
  subobjects respectively, and nothing degrades: no ceiling was reached and the
  cost is roughly linear, about 34 ms per extra pair of hit shaders on a cold
  compile. Each copy bakes a DISTINCT immediate so the driver cannot fold them
  into one.

  So the remaining work is engineering in the rewriter and the shader table,
  not another unknown about the driver.

  **What that leaves as a fix.** The shim knows the (geometryIndex,
  instanceContribution) pair for every record it writes, so the values can be
  BAKED IN as immediates instead of read: emit one hit group per distinct pair,
  with the constants folded into copies of the any-hit and the closest-hit, and
  drop the local root signature entirely. That removes the fatal construct
  rather than dodging it, and it needs no driver cooperation. The cost is one
  hit group and one pair of hit shaders per distinct pair, against a real
  Escher scene that wanted 27 records, and a larger library to compile.

  **BUILT IN 0.37.0, AND THE UNREAL LIBRARIES THAT CRASHED ARE CLEAN.** A new
  pass, `phase5/rewriter/bake.py` and `proxy/rewriter/rq_bake.cpp`, held
  byte-identical, runs AFTER `lower()`: every record read becomes
  `add i32 0, <value>`, every hit shader the module defines (`AnyHit`,
  `ClosestHit`, `Isect`, `ClosestHitProc`) is emitted once per pair as
  `<name>_<k>`, and the record's type, global, resource record and dead
  declarations are removed. It refuses its own output if a record read
  survives. `lower()` did not change, so nothing that reads no record moved by
  a byte.

  **A separate pass, because the pairs are per SCENE and lowering is per
  SHADER.** The acceleration structures do not exist when a pipeline is
  created, so the shim bakes `(0, 0)` then, which is exactly what the old table
  held before the first dispatch, keeps the original DXIL, and at the first
  dispatch needing a pair it lacks it lowers again, builds a new state object
  and retires the old one. The pair set only grows, like the table. Measured on
  `geomcontrib`: 1 to 5 pairs, 47 ms, 11 subobjects.

  Measured, cache cleared before every trial:

      the 15 Unreal libraries that read record constants    0 of 225
        (each was 30 to 80 percent before)
      crash.out.dxil, as it always was                       10 of 15
      crash_baked.out.dxil, the same shader through 0.37.0    0 of 15
      a real Unreal shader baked with 27 pairs               0 of 15

  **The render tests say the baking is RIGHT, not only survivable.** All 24
  dispatch cases bit-exact against WARP, including a new `bothgeom`, which
  gates every commit on the contribution its record reports so all four hit
  shaders are copied and the pipeline builds a triangle and a procedural
  group per pair. The sensitivity check was run, not assumed: forcing every
  record onto copy 0 makes `geom` and `geomcontrib` diverge by 4624 and
  `bothgeom` by 7396, exactly the procedural hit count. The per-record copy
  choice carries the result.

  Two things this brief should not have to rediscover. A dispatch can now
  rebuild the state object, so `DispatchAsRays` takes a per-pipeline lock and
  records from a snapshot; Unreal records on several threads. And the Escher
  run is still the test that matters: everything above is offline.

  **THE PREVIOUS BISECT MEASURED NOTHING, AND THE REASON IS WORTH MORE THAN
  THE RESULT IT DESTROYED.** `sotest` resolves the shape file with
  `libPath.rfind(".out.dxil")`, so a library named `var_base.dxil` gets no
  shape, and it refuses BEFORE `CreateStateObject` with exit code 1. The whole
  variant matrix was named that way. Every "8 of 8 CRASH" was eight shape
  refusals, identical for a good and a bad library, and `CreateStateObject`
  was never once called. Retracted in full:
  - "all three leads are dead" measured nothing, though sin/cos and f16 remain
    dead for a better reason: they are `BlueNoiseVec2` and `PackLightSample`,
    read out of the engine source, so that signature was a fingerprint of the
    shader family and never a construct;
  - "the generated any-hit is exonerated" measured nothing. The any-hit is in
    fact one of the two places the fatal read lives;
  - "the repro is DETERMINISTIC, 13 of 13" measured nothing. It is
    intermittent, about 40 to 80 percent per cold compile, exactly as the
    entry above it said. **The "probabilistic, wears off" model was withdrawn
    on the strength of a broken measurement and is hereby restored.**
  - the MegaLights family signature was small-n noise. 015 read 0 of 10 and is
    13 of 20; 011 and 014 were called safe and are 6 and 8 of 10.

  This is the same failure the brief already names twice, in a third place: a
  probe that fails identically for a good and a bad input measures nothing. It
  was caught by printing one full run instead of a counter, which is the
  cheapest check there is and was not done for a whole session.

  **THE HARNESS CONFOUND WAS CHECKED, NOT ASSUMED.** Clearing the cache raises
  the rate sharply (base 1 of 20 warm against 8 of 10 cold), and the clear is
  `rm -rf` on files the driver sometimes still holds, so "my own clear corrupts
  the cache" was a live alternative to "cold compiles crash". It is ruled out
  by content still mattering under identical clearing: four libraries are 0 of
  80 while others are 8 of 10, and an edit to one library moves it from 8 of 15
  to 0 of 15. A corrupting clear would hit everything alike.

  **The intermittency is what makes small n useless here**, and two readings in
  this investigation were built on n = 10 or less. Treat anything under 15
  trials as a hint. A variant that reaches zero is the only clean signal.

  **The bisect that settles it is now cheap, and one thing makes it cheaper
  than it looks: ANY EDIT IS AUTOMATICALLY A CACHE MISS.** Changing the
  library changes its bytes and therefore its cache key, so a variant gets a
  cold compile without clearing anything. What still needs the cache cleared
  is re-testing the SAME variant, because a compile that survives is cached.

  **There is now a repeatable experiment**, which there was not this morning:
  clear DXCache, sweep, count. That is the procedure any bisect of the library
  has to use, because a single clean run means nothing.

  It is also not size, and the numbers that used to say so were a warm
  cache. With the cache cleared every trial, 53852 bytes crashes 3 of 10
  and 9360 bytes is 0 of 20. Size tracks nothing; `recordconstants` does.

  **AND IT EXPLAINS WHY EVERY EARLIER OFFLINE REPLAY SUCCEEDED.** This brief
  built a whole conclusion on those: "every input is exonerated and the
  process is the whole difference", with four controls behind it, the suspect
  alone, sixty copies, 261 other state objects first, eight threads. Every one
  of them replayed the SAME SHADER, `RayTracingDebugMainCS`, because it was
  the only one the bisect had isolated. **The offline control was a sample of
  one, and that shader happens to be survivable on a quiet device.** The
  moment 0.36.7 let more shaders lower, a different one reproduced instantly.
  The conclusion is withdrawn: the fault is in something the LIBRARY contains,
  and the process context was never the variable.

  The method error is the familiar one in a new place. Four controls agreeing
  felt like four measurements and was one measurement repeated, because the
  input never varied. **A control has to vary the thing it is controlling
  for.**

  **What it buys is ITERATION.** Cutting the library down and asking which
  construct the driver cannot compile is now a one second experiment instead
  of a game launch. That is the whole difference between this being tractable
  and not.

- **THE BIGGEST REFUSAL IN A REAL GAME IS TWO SMALL THINGS, MEASURED FROM THE
  DUMP RATHER THAN GUESSED.** 64 refused shaders were on disk from one Escher
  run. Every one of the 58 "Proceed loop body reads values defined outside it"
  refusals was disassembled and the named values traced to their definitions.
  The answer is completely uniform:

      58 of 58 need  dx.op.annotateHandle
      24 of 58 also need  lshr

      what feeds the annotateHandle:   106 of 106  dx.op.createHandleFromHeap
      what the heap INDEX is:          106 of 106  extractvalue (a cbuffer field)
      what the lshr shifts by:          24 of 24   the constant 8

  So every single one is a **bindless resource handle whose descriptor index
  comes from a cbuffer**, and in 24 cases a constant right shift unpacking that
  index out of a packed field. Nothing else. No UAV reads, no phis, no genuine
  caller locals.

  **DONE IN 0.36.7, AND THE REPLAY SAYS 18 RATHER THAN 58.** Exempting 218
  and replaying the same 58 shaders offline:

      18  now LOWER, container in and signed container out, and byte-identical
          between the Python and the C++
      16  were ALSO doing a UAV append inside the loop, so they now hit the
          side-effect refusal from 0.36.6 instead
      24  still refuse: the chain runs through a rawBufferLoad or a textureLoad

  So the earlier reading of "two additions close all 58" was wrong in two
  ways, and both are worth keeping. `lshr` was already on the pure list, so it
  was never the missing piece; what those 24 actually need is a LOAD
  recomputed, which is a different and more delicate question because an SRV
  is safe to re-read and a UAV the raygen wrote is not, and recomputing
  hoists. And closing one refusal exposed the next for the fifth version
  running: 16 shaders were doing the same thing the debug shader does.

  **All of it is recomputable, by the argument this brief already makes twice.**
  A cbuffer holds the same bytes for the whole dispatch and the descriptor heap
  is the same heap in the any-hit as in the raygen, so the hit shader can
  rebuild the handle rather than be handed it. That is exactly why
  `createHandle`, `createHandleForLib`, `createHandleFromBinding` and cbuffer
  reads are already exempt. Two additions close the lot:

      createHandleFromHeap (218), when its index chain is recomputable
      lshr, by a constant amount

  **This brief already predicted half of it and filed it as correct.** The 0.25.0
  entry says "createHandleFromHeap (218) turns out to be sound as it stands...
  One used inside the Proceed loop is refused, because 218 is NOT on the
  recomputable list and the isolation check therefore catches it." True, and
  measured on a shader that kept all 16 in the raygen. The measurement now says
  that in-loop case is **the single largest refusal a real game produces**, 58 of
  157. A limitation recorded honestly is still a limitation, and nothing had
  counted it.

  The constant shift matters as much as the exemption and for the reason the
  brief gives for excluding division: recomputing HOISTS, and a variable shift
  amount can be undefined where the original was guarded. By a constant it
  cannot be.

- **THE CreateStateObject CRASH IS GONE, AND THE FAILURE MOVED THIRTEEN
  SECONDS LATER.** First run of 0.36.6 with no `rqlimit` and no `rqonly`, so
  every shader that lowers gets a state object:

      DXC:     dxcompiler 1.10.2605.37, dxil 1.10.2605.37     (checked first)
      163 RayQuery shaders seen
      6   state object BUILT
      1   REFUSED: Proceed loop body has a side effect (dx.op.bufferUpdateCounter, opcode 70)

  Exactly the predicted shape. The device survived every one of those builds,
  where the same configuration before the refusal built seven and died on the
  seventh. **That is the fix working, in the game, on the thing it was written
  for.**

  **What killed it instead is a DIFFERENT failure with a different error
  code.** The run reached acceleration structure building and rendering, ran
  for 19 seconds against about 6 before, and then:

      D3D12 ERROR #921: ID3D12CommandList::Close: An ID3D12Resource object was
                        deleted prior to closing the command list
      D3D12 ERROR #921: ... same resource, at ExecuteCommandLists
      D3D12 ERROR #232: RemoveDevice ... DXGI_ERROR_DEVICE_HUNG, TDR triggered

  `DEVICE_HUNG` is a timeout, not `DRIVER_INTERNAL_ERROR`. Both #921s name one
  resource, in the middle of UNREAL's own `BuildRaytracingAccelerationStructure`
  calls, and nothing the shim records sits near them: this scene took the
  upload-heap read path, so the shim recorded no copy into any list.

  **INFERRED, not established, and the shape of the experiment says to suspect
  it**: at `rqphase = 2` nothing lowered is ever dispatched, so 157 shaders get
  do-nothing pipelines and the engine then runs for 19 seconds consuming
  whatever was in those targets. An unwritten buffer read back as an indirect
  dispatch argument or a loop bound is an ordinary way to hang a GPU. The old
  crash at 6 seconds was masking whatever this is.

  **So the phase diagnostic has reached its limit.** It was built to answer
  "does building a state object kill the device", it answered that, and
  running the engine for 19 seconds on stubbed output is not a configuration
  worth debugging. The next step is the real path, with lowered shaders
  actually dispatching, and that needs the refusal question decided first:
  Unreal treats a forwarded refusal as fatal, so the 157 the rewriter still
  refuses would need do-nothing pipelines OUTSIDE a phase. That is a product
  decision, and this brief argues both sides of it already.

- **THE GPU CRASH BISECT, as far as it has got.** Each row is a real run of a
  shipping UE 5.8.2 game on the GTX 1070. `rqstub` and `rqphase` are in
  proxy/rq_pipeline.cpp and the example ini.

      nowrap = 1        translate nothing, device unwrapped      RUNS
      rqstub = 1        real do-nothing PSO, nothing lowered     RUNS
      rqphase = 1       + rewrite the DXIL, throw it away        RUNS
      rqphase = 2       + CreateStateObject                      CRASHES x2
      rqphase = 2, rqlimit = 1   exactly ONE state object built  RUNS, clean exit
      rqphase = 2, rqlimit = 4   four state objects built       RUNS, clean exit
      rqphase = 2, rqlimit = 6   six state objects built        RUNS, clean exit
      rqphase = 2, rqlimit = 7   all seven, the suspect last    CRASHES
      rqphase = 2, rqonly  = 6   ONLY the suspect, NOTHING else  CRASHES
      rqphase = 2, 0.36.6       suspect REFUSED, six built      no longer crashes,
                                                                hangs later instead
      (unset)           + shader table + dispatch                CRASHES

  **ONE STATE OBJECT DOES NOT KILL THE DEVICE, AND THAT IS THE FIRST REAL
  ANSWER THIS BISECT HAS PRODUCED.** Shim 0.36.2, `rqphase = 2`,
  `rqlimit = 1`, debug layer forced on: exactly **one** `state object BUILT`
  line, 162 refusals, and the game ran to its own clean `end:` marker. So
  `CreateStateObject` on a generated library, inside Escher's process, is
  survivable once. The suspect that survived five versions of elimination is
  not a single call.

  **AND THE REMAINING RANGE IS TINY, WHICH NOBODY EXPECTED.** Of those 163
  RayQuery shaders only **SEVEN lower at all**, measured for the first time
  now that the rewriter sees every one of them:

      120  Proceed loop body reads values defined outside it
       35  2 concurrent RayQuery objects
        7  LOWERED  (1 built, 6 held back by rqlimit = 1)
        1  assembler: use of undefined value '%v111' on CommittedRayT (200)

  So "unlimited at phase 2" means seven state objects, and the crash lives in
  2..7. That is two or three runs of bisect, not an open-ended search.

  **FOUR DO NOT KILL IT EITHER.** Same configuration with `rqlimit = 4`: four
  `state object BUILT` lines, clean `end:` marker. So the crash is the 5th,
  6th or 7th of the seven, and it is now a choice between three named shaders
  rather than a property of the call.

  **THE BISECT IS DOWN TO ONE SHADER.** `rqlimit = 6` built six and ran to a
  clean exit, so it is the SEVENTH to lower. It stands out physically, which
  none of the reasoning predicted: **13132 bytes of input against 3968 to 6004
  for the six that are fine**, lowering to a 28984 byte library. It is on disk
  as `archive/lowered-rqlimit-6/SUSPECT_7th.in.dxil`, and it lowers offline
  without complaint, pattern 3, alpha-tested with a generated any-hit.

  **AN OFFLINE PROBE OF IT LOOKED LIKE AN INSTANT ANSWER AND WAS NOTHING.**
  `sotest` on the suspect's library gave `CreateStateObject E_INVALIDARG` on
  the 1070, which is exactly the result the whole investigation has been
  looking for. **The control killed it:** one of the SIX that built fine in the
  game gives the identical `E_INVALIDARG` when run the same way. The cause is
  that the shim only writes a `.rs.bin` for a shader it actually BUILDS, so a
  shader held back by `rqlimit` is dumped without its root signature, and no
  DXR state object builds without a global root signature. **A probe that
  fails identically for a good and a bad input measures nothing**, which is the
  same rule this brief already states for an oracle that stays silent.

  So the faithful replay needs `rqlimit = 7`, which builds the suspect and
  therefore dumps its `.rs.bin`. The dump is written BEFORE
  `CreateStateObject`, so that set survives even when the device dies on the
  call.

  **THE BISECT FINISHED, AND IT NAMED ONE CALL TO THE INSTANT.** `rqlimit = 7`
  crashed. Six `state object BUILT` lines, no seventh, no logged
  `CreateStateObject` failure, and then, 337 ms later, the last line in the
  log:

      device QI PASSED THROUGH UNWRAPPED: {9727A022-CF1D-4DDA-9EBA-EFFA653FC506}

  That IID is **`ID3D12DeviceRemovedExtendedData1`**, checked in the Windows
  SDK `d3d12.idl`, so it is Unreal's own device-removed handler collecting
  DRED. The device was already gone by then. The seventh `CreateStateObject`
  is the call that does it, and nothing else ran in between.

  **AND THE SEVENTH IS NOT POISON. THE SAME CALL SUCCEEDS OFFLINE.** The dump
  is written before `CreateStateObject`, so `lowered_006.*` survived with its
  `.rs.bin` this time. Replayed on the same 1070 with `sotest`:

      lowered_006 alone, with its own root signature   hr=0x00000000
      all seven, in order, held at once                0 failed, device alive

  So the library is not the variable, the root signature is not the variable,
  and the count is not the variable. **Everything about the call is
  reproducible and benign outside Escher's process.** That is the same
  contradiction this brief already records, now narrowed from "somewhere in
  the run" to one call whose inputs are all on disk.

  **IT IS ONE SHADER, ON ITS OWN, AND IT HAS A NAME.** `rqonly = 6` built
  **zero** other state objects and the device died at exactly the same place.
  Every run now lines up without an exception:

      rqlimit = 4   suspect REFUSED   4 built   163 shaders seen   RUNS
      rqlimit = 6   suspect REFUSED   6 built   163 shaders seen   RUNS
      rqlimit = 7   suspect BUILT     6 built    11 shaders seen   CRASHES
      rqonly  = 6   suspect BUILT     0 built    11 shaders seen   CRASHES

  The crash happens if and only if that one shader gets a state object, and
  it needs no company at all. The count was never the variable, and the two
  surviving runs only survived because `rqlimit` happened to refuse the
  suspect.

  **The shader is `RayTracingDebugMainCS`**, read out of the dumped container.
  It is the SAME shader this brief already records as the first real Unreal
  shader to lower, validate and sign, in 0.25.0, and the one the
  `createHandleFromHeap` finding was measured on, 16 heap handles all kept in
  the raygen. 13132 bytes in, 28984 out, more than twice the largest of the
  six that are harmless.

  **THE DEBUG LAYER SAYS NOTHING ABOUT THE CALL, AND THAT IS ITSELF THE
  RESULT.** 0.36.4 relays the layer into this log through
  `ID3D12InfoQueue1::RegisterMessageCallback`, which delivers messages
  synchronously and is therefore the only way to read one produced inside a
  call that never returns. The end of the log:

      23:48:12.810  about to call CreateStateObject: 28984 byte library, 9 subobjects
      23:48:12.847  device QI {9727A022-...}            ID3D12DeviceRemovedExtendedData1
      23:48:12.847  D3D12 WARNING #233: RemoveDevice ... DXGI_ERROR_DRIVER_INTERNAL_ERROR

  The layer's entire vocabulary that run was 24 `native 16bit ops` errors of
  Unreal's own, 31 pixel-shader RTV warnings, 4 resource-state warnings and
  that one removal. **Zero validation complaints about our state object.** So
  D3D12 is happy with the description and the fault is inside the driver,
  compiling a library the runtime accepts, 37 ms in.

  **THREE MORE OFFLINE CONTROLS, ALL NEGATIVE, ALL WORTH NOT REPEATING.** On
  the same 1070, device alive every time:

      the suspect alone                            builds
      the suspect x60, all held at once            builds
      260 other state objects, then the suspect    builds

  **The 261 one nearly did not count.** `sotest`'s directory walk capped at
  64 files, so the first attempt silently built 64 and reported "device still
  alive". A cap that turns a test into a weaker test without saying so is the
  same failure as a knob that does not move what it names, which this
  investigation has now been caught by twice. Raised to 1024.

  **So every input is exonerated and the process is the whole difference.**
  Same library bytes, same root signature, same `D3D12Core.dll 1.618.5.0`,
  same GPU, same driver: offline it builds and the device lives, in Escher it
  removes the device. Nothing left to vary except what that device has
  already been asked to do.

  **THE MEMORY IDEA IS DEAD, IN ONE RUN, WHICH IS WHAT IT WAS FOR.**

      video memory before CreateStateObject: LOCAL usage 520 MB of budget 7299 MB (7%)
      video memory before CreateStateObject: SYSTEM usage 100 MB of budget 9423 MB

  Seven percent. Not pressure, not close to it.

  **TWO MORE DEAD, BOTH CHEAP.** 336 state objects built across 8 threads, 48
  of them the suspect, device alive: driver concurrency among our own builds
  is not it. And no NVAPI shader extension markers in any of the seven
  containers, so the driver's extension pattern matching is not involved
  either, which would have explained the offline/in-game split neatly and does
  not.

  **THE SHADER, READ FROM THE ENGINE SOURCE.**
  `Engine/Shaders/Private/RayTracing/RayTracingDebug.usf`,
  `RAY_TRACING_ENTRY_RAYGEN_OR_INLINE(RayTracingDebugMain)`, the
  `RAYTRACING_DEBUG_INLINE` arm, which is `#define`d to `COMPUTESHADER`. Three
  things worth having:
  - **It is the ray tracing DEBUG VISUALISATION pass.** Nothing a game renders
    normally depends on it. That matters more than any diagnostic below.
  - Its ray flags are computed at RUNTIME, `CULL_BACK_FACING_TRIANGLES` plus
    `FORCE_OPAQUE` or `FORCE_NON_OPAQUE` chosen by `VisualizationMode`. The
    rewriter supports that since the `RayFlags` work.
  - It is big because of what happens AFTER the trace: `GetInstanceSceneData`,
    Nanite triangle attribute loading through
    `InstanceContributionToHitGroupIndex` and `GeometryIndex`, and a
    world-to-object matrix. That is the 13132 bytes and the
    `recordconstants=1`.
  - CAVEAT: the local clone is UE 5.7 NvRTX and Escher is 5.8.2, so the
    permutation that shipped may differ in detail.

  **AND THERE IS EXACTLY ONE STRUCTURAL FEATURE THAT SEPARATES THE SUSPECT
  FROM ALL SIX HARMLESS SHADERS.** Every one of the seven was disassembled and
  compared:

      lowered_000    487 ll-lines   heapHandles= 7   bufferUpdateCounter=0
      lowered_001    366             2                                   0
      lowered_002    375             2                                   0
      lowered_003    411             5                                   0
      lowered_004    645             6                                   0
      lowered_005    645             6                                   0
      SUSPECT       2001            16                                   4

  Bindless is NOT the discriminator: all seven use `createHandleFromHeap`, the
  suspect just uses more. **`dx.op.bufferUpdateCounter` (70) is**, four
  `(uav,inc)` increments in the body that becomes the RAYGEN, and zero in
  every shader that is fine. That is a UAV counter allocation, the
  `IncrementCounter`/`Append` idiom, ending up inside a DXR 1.0 raygen on
  Pascal's emulated path.

  **AND THE COUNTER IS INSIDE THE PROCEED LOOP, WHICH MAKES IT A REWRITER
  DEFECT AND NOT JUST A CLUE.** Confirmed against the UE 5.8.2 source Escher
  was built from,
  `IMPLEMENT_GLOBAL_SHADER(FRayTracingDebugCS, ".../RayTracingDebug.usf",
  "RayTracingDebugMainCS", SF_Compute)`. The generated `AnyHit`, in full, is
  the loop body, and it carries:

      %v233 = annotateHandle ... RWStructuredBuffer<stride=256, counter>
      %v234 = bufferUpdateCounter(70, %v233, inc)
              rawBufferStore(140, %v235, %v234, 0,  94, ...)
              rawBufferStore(140, %v236, %v234, 76, %v232, ...)
      %v238 = bufferUpdateCounter(70, %v237, inc)

  **A Proceed loop body with SIDE EFFECTS is not transplantable into an
  any-hit shader, and the rewriter transplants it anyway.** The two run a
  different number of times, by design:
  - with `RAY_FLAG_FORCE_OPAQUE`, which this shader sets at RUNTIME for every
    mode but one, the any-hit never runs at all and the records are never
    appended;
  - with `FORCE_NON_OPAQUE` it runs once per candidate, in an
    implementation-defined order, so the records appear in a different order
    and a different count.

  So the lowering silently changes an observable. This is the same class as
  the loop isolation rule and the rule MISSED it: that rule guards what the
  body READS, and this brief even says "no loads, because a UAV the raygen
  wrote reads back differently". Nothing guards what the body WRITES. Every
  shader in the suite has a pure loop body, so nothing here could expose it,
  which is the fourth time a suite of cases written to demonstrate a lowering
  shared its author's blind spot.

  **THE REFUSAL IS IN, 0.36.6**: a Proceed loop body that stores to a UAV,
  updates a counter or does an atomic cannot become an any-hit shader.
  That is correct on its own terms, and it also stops this shim ever building
  the library that kills the device, which is a symptom fix arriving for an
  unrelated and better reason.

  **The rule is read from the module, not from a list.** DXC marks every
  `dx.op` declaration with an attribute group and the module says which is
  which: `readnone` is pure, `readonly` loads, a bare `nounwind` WRITES. A
  hand-kept list of store opcodes would be incomplete the day DXIL grows
  another one. The numbering is resolved per module rather than assumed, which
  is the attribute group bug 0.26.0 already had to fix. `rayQuery_*` ops are
  exempt because they are rewritten rather than transplanted, and the analysis
  already refuses an opcode it does not know.

  Both implementations, byte-identical, and all thirteen render cases produce
  identical output, so nothing existing moved. The suspect now refuses, word
  for word the same in the Python and the C++, and the other six still lower.

  **The test took two goes and the first one was the useful one.** Adding the
  counter update on a UAV handle made `refuse_uav_in_loop` fire instead, so
  the case proved the OLD refusal. It had to go on the handle the loop already
  reads through, which the isolation check exempts, before the new refusal was
  the one being provoked. A refusal test that fires the wrong refusal passes
  and measures nothing.

  **STATED HONESTLY: this is a correlation, not a demonstrated cause.** The
  same library still builds offline, so the counter alone is not sufficient.
  But it is the first thing in this whole investigation that cleanly
  separates the shader that kills the device from the six that do not, and the
  refusal above is worth making whether or not it turns out to be the cause.

  **The offline control was re-checked rather than trusted**, because
  everything rests on it. Twenty builds of the suspect take 1728 ms against
  1542 ms for twenty of a harmless one, so the driver is doing real, size-
  dependent compilation work offline and not returning a cached or deferred
  answer. The control is genuine: the same library really does compile on this
  card and really does kill the device in Escher.

  **261 of Unreal's own DXR state objects are alive when the call is made**,
  counted from the log. Memory is the first thing about a loaded game that an
  empty probe cannot reproduce, and a driver that cannot allocate while
  compiling a shader is a plausible source of an error code that carries no
  detail. 0.36.5 logs `QueryVideoMemoryInfo` immediately before the call. If
  usage is nowhere near budget the idea dies in one run, which is worth more
  than it being right.

  **What is left to separate, and `rqonly` is the instrument.** 0.36.3 adds
  `rqonly = N`: build ONLY the N-th shader that lowers, counting from 0,
  refusing every other one even though it lowered. `rqlimit` can only ask how
  many, and the question is no longer a number. `rqonly = 6` builds the
  suspect on its own, so either that shader alone kills this process, which
  puts the difference entirely in the process context, or it needs the other
  six present, which makes it an interaction and lets `rqonly` walk 0..5 to
  find the partner.

  **THE SET OF SEVEN IS STABLE, WHICH THIS BISECT DEPENDS ON AND NOBODY HAD
  CHECKED.** Unreal creates compute PSOs on worker threads, so "the first N to
  lower" could in principle be a different N each run. Two things say it is
  not: the refusal composition came back identical, 120 / 35 / 7 / 1, and the
  first shader to lower dumped byte-for-byte identical in both runs, 5144
  bytes with the same generated shape. The `lowered_*.in.dxil` files are the
  instrument for this: hash them every run and the ordering is checkable
  rather than assumed.

  **The refusal composition is itself new**, because every previous count was
  taken with 162 shaders forwarded unexamined. The 120 and the 35 are the two
  known false refusals, loop isolation and the allocation count, so the real
  lowering rate is far higher than seven; seven is what TODAY's rewriter
  manages, not what the shaders permit.

  **THE `rqlimit = 1` ROW PROVED NOTHING FOR TWO RUNS BEFORE THAT, AND THE SHIM
  WAS WHY.** Under any
  phase, 0.36.0 gave a REFUSED shader a do-nothing pipeline by returning BEFORE
  the log line and before `shdump::Refused`. So both outcomes were silent and
  the run could not say whether its one allowed shader lowered and built a
  state object or was simply refused. The dump never initialised either, which
  is the tell: `shdump::Lowered` runs before `CreateStateObject` and would have
  logged, so the shader was probably refused and **no state object was ever
  built**. Probably is not good enough, so 0.36.1 logs and dumps every outcome
  under a phase and the run is being repeated.

  **Same mistake as the last two, in a new place.** 0.36.0 made the phases
  comparable and in doing so removed the instrument that made them readable.
  The brief already says: when you change what the product DOES, re-ask what
  each test is still measuring.

  **AND THE REPEAT BUILT NOTHING EITHER, FOR A SECOND, DIFFERENT REASON.** With
  0.36.1 logging every outcome, the rerun read: 163 RayQuery shaders, 162
  refused by `rqlimit`, one refused by the rewriter, and **zero state objects
  built**. `rqlimit` counted shaders that REACHED the rewriter rather than
  shaders that LOWERED, and those are not the same set, because the first
  shader Unreal creates is not the first one that lowers. The whole budget went
  to a shader the rewriter then refused for loop isolation on `%v119`. Fixed in
  0.36.2: the gate sits after the rewrite, so `rqlimit = N` means at most N
  state objects.

  **Three runs asked "does one state object kill it" and none of them built
  one**, and the fourth answered it. The lesson is not about `rqlimit`. It is
  that a bisect knob has to be shown to MOVE the thing it names, and none of
  those three did, because the log could not report what the knob had actually
  done until it was made to. Two of them were then read as results.

  **THE SHADER DUMP HAS NOTHING TO DO WITH IT, AND THE "PERFECT CORRELATION"
  WAS AN ARTEFACT OF NOT READING THE LOG.** `shader dumping is ON` appears
  ZERO times across every run in the log. Dumping was never enabled in any of
  them, so `shdump::Lowered` took a mutex, saw an empty directory and returned,
  in the runs that crashed and the runs that did not, alike. It wrote nothing
  and it is not a variable. The row above used to read "+ shader dump +
  CreateStateObject" and that description was simply false.

  **So the variable between phase 1 and phase 2 is `CreateStateObject` on a
  generated library, in the game's process, and nothing else.** Two runs at
  `rqphase = 2` (pids 5396 and 1880) died with no end marker. Three runs at
  `rqstub = 1` and one at `rqphase = 1` ended cleanly. The rewrite is
  innocent, the shader table and the dispatch are never reached, and the call
  that does it is the one call this project had already ruled out offline.

  **That is the contradiction to work on, and it is now sharp.** The same
  libraries build on the 1070 one at a time, 210 in sequence, 168 across eight
  threads, on the game's own `D3D12Core.dll`, and on WARP, with the device
  alive afterwards. In Escher's process the first ones kill it. So the defect
  is not in the library and not in the call, it is in the CONTEXT: what else
  this device has been asked to do by the time the call is made. An offline
  probe cannot reach that by construction, which is why the next instrument is
  a Development build with `WITH_RHI_BREADCRUMBS` rather than another replay.

  **Method note, third time in this investigation and the most expensive.**
  The conclusion "the dump is the cause" was written into this brief and then
  withdrawn within the hour, because a run was assumed to have used the
  configuration it was asked to use. The shim logs its own configuration on
  every run, in one line, precisely so that never has to be assumed. **Read
  the log line that names the configuration before interpreting the run.**

  **What is ruled out, by measurement, and is not worth testing again:**
  - the generated libraries: each builds on the 1070, all seven at once, 210 in
    sequence, 168 across eight threads, and on WARP;
  - the D3D12 runtime: `sotest` exports `D3D12SDKVersion`/`D3D12SDKPath` and
    loads the game's own `D3D12Core.dll 1.618.5.0`, same result;
  - Unreal driving ray tracing on Pascal: `rqstub` leaves that entirely intact;
  - the stand-in pipeline state: it is a real one since 0.34.0;
  - running DXC in the game's process: `rqphase = 1` runs;
  - the second executable: `Escher.exe` is `BootstrapPackagedGame`, 176 KB,
    which `CreateProcess`es the Win64 binary and imports no d3d12 or dxgi.

  **What DRED says, and what it does NOT.** `-gpucrashdebugging` turns it on,
  `RHI.DRED true`, and the report has no breadcrumbs and no page fault data,
  with Aftermath on and writing no dump.

  **The missing breadcrumbs mean nothing, and reading them as evidence was an
  error.** This is a SHIPPING build, and

      WITH_RHI_BREADCRUMBS = (UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT ||
                              WITH_PROFILEGPU || (HAS_GPU_STATS && RHI_NEW_GPU_PROFILER))
      WITH_PROFILEGPU      = !(UE_BUILD_SHIPPING || UE_BUILD_TEST) || ...
      HAS_GPU_STATS        = ((STATS || CSV_PROFILER_STATS ||
                               GPUPROFILERTRACE_ENABLED) && (!UE_BUILD_SHIPPING))

  Every term is off in Shipping, and `HAS_GPU_STATS` carries its own
  `!UE_BUILD_SHIPPING`, so this is not a matter of which profiling options the
  packager happened to enable. Unreal writes no breadcrumbs at all in Shipping. DRED had nothing to
  report because nothing was recorded, not because the GPU was idle. The
  inference "nothing was executing, so this is a CPU-side failure" does not
  follow and is withdrawn.

  What still holds: no page fault was recorded, which comes from the runtime
  rather than from Unreal's instrumentation.

  **The general lesson, and it cost a whole line of reasoning**: a Shipping
  build strips the instruments, so silence from any of them is the absence of
  a measurement and not a measurement of absence. The same rule this brief
  already states for the debug layer, applied to the wrong tool.

  **An instrument Shipping cannot strip** is the D3D12 debug layer, forced on
  for a named executable with `dxcpl.exe`. It lives in the runtime, not in the
  application, so the build configuration cannot remove it, and API misuse by
  this shim is exactly what it reports.

  **Two experiments were wrong before they were right, the same way twice.**
  `rqlimit = 0` and the first `rqphase = 1` both ended on Unreal's "Shader
  compilation failures are Fatal", because a forwarded RayQuery shader is fatal
  in Unreal, so the run stopped well before the point being bisected. A phase
  now hands a refused shader a do-nothing pipeline too. **The note explaining
  why this would happen was already in this brief when the second one was
  written.**
- **THE DEBUG LAYER NAMED SOMETHING, AND IT IS NOT WHAT ANY THEORY PREDICTED.**
  Forced on with `dxcpl.exe` and read with DbgView, on the run that crashes:

      D3D12 ERROR: ID3D12Device::CreateComputeShader: Shader uses native 16bit
      ops, but the device does not support this.
      [ STATE_CREATION ERROR #622: CREATESHADER_INVALIDBYTECODE ]     x24
      D3D12: Removing Device.
      D3D12 WARNING: RemoveDevice: DXGI_ERROR_DRIVER_INTERNAL_ERROR

  Twenty-four compute shaders using native 16-bit ops, which Pascal does not
  support, rejected one after another, and then the device goes. The removal
  warning even says it: "strong evidence that the driver has performed an
  undefined operation; but it may be because the application performed an
  illegal or undefined operation to begin with".

  **This shim answers the capability truthfully.** `CheckFeatureSupport` is
  forwarded untouched except for `OPTIONS5.RaytracingTier`, so
  `D3D12_FEATURE_DATA_D3D12_OPTIONS4.Native16BitShaderOpsSupported` reaches
  Unreal as FALSE, and `D3D12Adapter.cpp` sets
  `GRHIGlobals.SupportsNative16BitOps` straight from it. Unreal knows, and
  creates them anyway, so the permutation choice for these particular shaders
  does not consult it.

  **The only renderer consumer of that global is TSR**, in
  `TemporalSuperResolution.cpp`, and it is gated on
  `bSupportsRealTypes == RuntimeGuaranteed` OR the capability being true, so on
  Pascal with `RuntimeDependent` it should already decline. Which means these
  24 are probably something else, and guessing which is how the last day went.

  **THE CONTROL RAN, AND THE 24 ERRORS ARE NOT THE CAUSE.** Same game, same
  forced debug layer, `rqstub = 1`: all 24 `CreateComputeShader` errors appear,
  one after another, and the run CARRIES ON. No `Removing Device` anywhere.
  After them it builds 8 bottom-level structures, reads a top-level one,
  emulates two AddToStateObject calls and exits through the shim's own end
  marker. A configuration that lives through those errors cannot be killed by
  them.

  So D3D12 rejecting those 24 shaders is something Unreal does on this card
  whatever the shim is doing, and the device removal in the full run has a
  different cause. That is the third theory retired by a control rather than by
  argument, and the control cost one run.

  **What `rqstub` looks like on screen, and it is the diagnostic working.** The
  frontend renders at wild brightness with the hue shifted, slow and stuttery.
  Expected: every RayQuery shader is a do-nothing PSO, so Lumen and MegaLights
  read whatever was in their targets, and the forced debug layer costs the rest.
  It does say one useful thing, that Unreal genuinely CONSUMES those outputs
  rather than having a fallback, so a correct lowering has somewhere to show.

  Toggling ray tracing and MegaLights in the menu ended that run. Under
  `rqstub`, where nothing is lowered, so it is not the translation. Noted and
  not chased: the toggle tears down and rebuilds the whole ray tracing
  pipeline, and the STARTUP crash is the one being bisected.

  **The tool worked because it is in the runtime.** A Shipping build strips
  Unreal's own instrumentation, which is why DRED had nothing; `dxcpl` forces a
  layer the application cannot compile out. Mute Info, keep Warning, Error and
  Corruption.
- **THE 33 "CONCURRENT" QUERIES ARE SEQUENTIAL. The refusal is false.**
  Confirmed from `refused_002.dxil`, NiagaraCollisionRayTraceCS:

      %120  allocate line 202, traced 203, last read 227
      %379  allocate line 521, traced 522, last read 546

  The first query is finished three hundred lines before the second is
  allocated. They never overlap. The check refuses on
  `FindOp(kAllocate).size() != 1`, which counts ALLOCATIONS, not liveness, and
  the message says "concurrent" about something it never measured. Same shape
  as the alphaMask over-refusal and the loop-isolation one.

  Lowering them is still real work, because each query needs its own TraceRay
  and its own generated hit shaders, but "no lowering exists" was wrong.

- **So all 157 refusals are now explained, and none of them is "this cannot be
  done".** 124 are one shader model gap, 33 are one false refusal.

- **Unreal's refusals as of 0.23.0**, with every assembler cause but one
  closed: 118 loop isolation, 33 concurrent queries, 6 unused `rq_cbv0`, 1
  assembler. The isolation ones now name real VALUES rather than block labels,
  so they are genuine caller locals and not the phi bug.
  - **Two reproductions of the unused-`rq_cbv0` refusal FAILED**, and that is
    worth recording. A cbuffer read only inside the Proceed loop lowers and
    signs, because the hit shader rebuilds the handle and keeps it used. A
    cbuffer declared and never read is eliminated by DXC before the rewriter
    sees it. So it is neither obvious shape, and the next guess would be a
    guess.
  - The right next move for ALL THREE is the dump, not more reasoning.

- **Refused shaders can be DUMPED now**, `dump` in the ini or
  `DXR_TIER11_DUMP`: a `.dxil` container per refusal with a `.txt` saying why,
  off by default, capped at 64. `dxrw rewrite` takes one back and reproduces
  the same refusal offline, so a shader that only exists inside somebody's game
  process can be disassembled and read. Built because the concurrent-query
  refusal cannot be settled from a log line.

- **The "2 concurrent RayQuery objects" refusal counts ALLOCATIONS, not
  overlap.** `rq_analyze.cpp` refuses when `FindOp(kAllocate).size() != 1`. Two
  queries used one after the other, never live at the same time, are refused
  identically. Same shape as the `alphaMask` over-refusal: right in principle,
  wrong for the case in front of it, and it is costing 33 of Unreal's shaders.
  Whether Lumen's two are really concurrent needs the DXIL, which means dumping
  refused shaders to disk.

- **The transparency check has a reliability signal, and it is the frame
  count.** `run_proxy_test.ps1` reported 14381 of 14400 pixels differing twice
  in a row, in a path the change could not reach. Both bad runs had captured 2
  and 5 frames instead of 8; three consecutive runs at 8 frames all reported 0.
  A run that captured fewer than 8 frames should not be believed either way.

- **In Unreal, a refusal is a crash.** The shim claims Tier 1.1, refuses a
  shader, forwards it unchanged so the driver gives its own error, and Unreal
  turns that into `LowLevelFatalError ... Shader compilation failures are
  Fatal`. "Refuse loudly and let the driver decide" was right when the
  alternative was a wrong render; against a shipping game it turns a missing
  feature into a game that will not launch. **The answer is to lower them, not
  to degrade them:** the premise of this project is to emulate Tier 1.1, and a
  shim that switches ray tracing features off has not emulated anything.

- **FIRST REAL ENGINE RESULT (2026-09-22).** Unreal Engine 5.8.2, a shipping
  game, on the GTX 1070, with both proxies installed: **8 bottom-level
  acceleration structures built, 208 DXR state objects created, all
  `hr=0x00000000`, and one AddToStateObject emulated, 201 + 13 into 213
  subobjects.** The DXR 1.0 path works at engine scale, not just on this
  project's own harness. What stopped it there was the stream-form PSO gap
  below, not anything in the lowering.

  **The AS tracking has now read a real scene, and the numbers are the first
  hard requirement a game has placed on the shader table.** The `rqstub` run
  resolved the instance buffer out of 306 tracked buffers and reported **6
  instances, max `InstanceContributionToHitGroupIndex` 26, triangles only, 0
  instances pointing at an unseen bottom-level structure**, so a lowered
  dispatch on that scene needs **27 hit group records** rather than one. The
  growth path exists and is tested; this is the first measurement of how far it
  has to grow in something nobody wrote for this project. The upload-heap read
  path served it, so nothing waited.

- **Two defects found by pointing it at real engine shaders, NOT yet fixed.**
  Investigating Falcor and RTXPT as targets turned these up in an hour, and
  each would meet a real application before any of the interesting refusals do:
  - **A RayQuery shader with no early-out bounds check does not lower.** The
    entry block becomes a phi predecessor and normalization leaves a dangling
    `%bb0`, because the entry block has no label line to rename. It fails at
    the ASSEMBLER with `use of undefined value '%bb0'`, which names nothing
    relevant. Reproduced minimally on a plain SM 6.5 non-bindless shader.
    **Every shader in the suite opens with `if (tid.x >= width) return;`**, and
    that one shared habit hid it. The probe is
    `phase5/cases/reference/rq_bindless_probe.hlsl`.
  - **`CreatePipelineState`, the pipeline stream form: FIXED, and it was worse
    than this said.** It did not detect either. The walker gave up at the first
    subobject that was not a shader, and a real engine's compute stream puts
    ROOT_SIGNATURE first, so it never reached the CS. Every RayQuery shader in
    a real game went to the Tier 1.0 driver unexamined, the driver rejected
    them, and Unreal treats a failed compute PSO as FATAL. Now
    `proxy/pso_stream.{h,cpp}`, with the same substitution the struct form has,
    covered by the `stream` case in the dispatch suite.
    - **A subobject's payload sits at the natural alignment of its inner type,
      not at a fixed offset.** `{tag(4); UINT}` puts the payload at 4 and is 8
      bytes; `{tag(4); pad(4); ptr}` puts it at 8 and is 16. A fixed offset
      desynchronises on the first NODE_MASK. Caught on the first run of the new
      test, which reported stopping at subobject type 919831536, a fragment of
      a pointer. Sizes come from `sizeof`/`alignof` so the compiler owns them.
    - **The suite had only ever used the struct form**, which is exactly how
      this survived. A new entry point is a new case, not a variation.
  - **`createHandleFromHeap` is documented as refused and is not. RESOLVED in
    0.25.0, and the documented refusal was the thing that was wrong**: passing
    a raygen-only heap handle through is correct, and an in-loop one is already
    caught by the isolation check. See the 6.6 entry above.

- **What a survey of real engines actually found**, and it reframes the
  remaining work:
  - **`GeometryIndex` is the standard hit-identification idiom.** Falcor
    identifies every hit as `GeometryInstanceID(InstanceID(), GeometryIndex())`,
    and `Committed`/`CandidateGeometryIndex` appear on nearly every inline
    path. So the one accessor measured as impossible is the one real engines
    use most. RTXPT's intro sample uses it too.
  - **But it is no longer impossible.** It was written off before the shader
    table machinery existed. Set
    `MultiplierForGeometryContributionToShaderIndex` to 1, size the table by
    the geometry counts `as_tracker` already records per BLAS, and give each
    record a local root signature carrying its geometry index as a root
    constant. The closest-hit reads that constant. Ordinary DXR 1.0 practice,
    and every piece exists except the local root signature.
  - **Both NVIDIA samples put RayQuery inside a RAYGEN shader**, not a compute
    shader. Measured: `PathTracerSample.hlsl` and `IntroPathTracer.hlsl` are
    `[shader("raygeneration")]` with a RayQuery inside, and no compute entry
    points anywhere in RTXPT's shader set.
  - **Our refusal for that is broader than its own stated reason.** CLAUDE.md
    justifies refusing RayQuery inside a DXR 1.0 shader with "any-hit and
    intersection shaders cannot call TraceRay". True of those two. **A raygen
    CAN call TraceRay.** So raygen-hosted RayQuery is lowerable in principle,
    and it is the single most common real shape. The obstacle is not the
    shaders, it is SHADER TABLE OWNERSHIP: in the compute path the shim builds
    the table, in the raygen path the application does, so generated hit groups
    need records appended to a table we do not own.
  - Falcor's `VBufferRT.cs.slang` IS a pure compute inline pass, 16x16, early
    out, alpha test in the loop. It is the closest real target, and it needs
    the `GeometryIndex` work.
  - RTXPT's driver minimum of 595.71 is NOT the blocker it looks like: its
    README documents dropping Agility SDK 1.619 at configure time, which drops
    it to DXR 1.1. A DXR 1.1 requirement is the TARGET, not a cost. DXR 1.2,
    SM 6.9, SER and OMM are the genuinely out-of-scope parts.

  Next action. **Not a feature: exposure.** Run real software through it, until
  the refusal list is trusted. All three defects named above are now fixed, and
  the ordered list of what remains is at the top of this brief.

  **No accessor is permanently refused any more.** The four that were,
  Candidate and `CommittedGeometryIndex` and the two
  `*InstanceContributionToHitGroupIndex` forms, all work through local root
  signature constants in the shader table records. The paragraph that used to
  stand here said they must keep failing loudly, and it was right for as long
  as the application owned the table.

  What is NOT generic, written down so it is not rediscovered:
  - Resource arrays work with a constant index, a dynamic one, and a
    non-uniform one, in the Shader Model 6.5 binding form. The 6.6 form of a
    DYNAMIC array index is refused, because the shape of an array global in
    the binding form has not been measured off DXC. A dynamically indexed
    handle used INSIDE the Proceed loop is refused in either form, because the
    index is raygen state the any-hit cannot see.
  - `createHandleFromHeap` (218) needs no special handling and has none. A heap
    handle used only in the raygen passes through correctly; one used inside
    the Proceed loop is refused, because 218 is not on the recomputable list
    and the isolation check catches it.
  - The payload layout is FIXED, now 92 bytes, and carries exactly the
    committed accessors the whitelist supports. It is a state object contract:
    `PAYLOAD_BYTES` and `MaxPayloadSizeInBytes` move together or
    `CreateStateObject` fails. It lives in FOUR places, `lower.py`,
    `rq_lower.cpp`, `rq_pipeline.cpp` and the harness.
  - One entry point, one query.
  - Procedural primitives lower, via a generated intersection shader, and so
    does a query committing BOTH kinds, via an any-hit and an intersection
    shader from one loop body. A SCENE holding both kinds is fine too, unless
    the application routed both to the SAME hit group record, which is DETECTED
    from the instance data and refused rather than drawn wrong.

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
  correct. DXR_TIER11_NOWRAP=1 disables wrapping, restoring forward-only
  behaviour.

Dev machine (Windows x64), everything under `C:\DW`:
- `C:\DW\dxr11-pascal` - this repository, published as `pascal-dxr-tier-1.1`.
- `C:\DW\DXC` - DirectX Shader Compiler (`inc\dxcapi.h`, `bin\x64\dxcompiler.dll`,
  `dxil.dll`).
- `C:\DW\microsoft.direct3d.d3d12.1.619.5` - Agility SDK.
- `C:\DW\DirectX-Graphics-Samples` - Microsoft samples, source of the DXR 1.0
  app used to validate the proxy.
- Build scripts: `build.bat` (phase 1), `build_phase2.bat`, `build_proxy.bat`,
  `build_dxgi.bat`, `build_sample.bat`, `build_phase4.bat`.
  `build_dxgi.bat` builds the SECOND proxy, `dxgi.dll`, which exists only to
  report a Pascal card under a Turing device id. Separate from `d3d12.dll` so
  it can be removed on its own, but not optional in practice for Unreal.
  `tools\dxr-tier-11-setup.bat` is the end-user installer, a WinForms
  PowerShell script: pick the .exe, install, toggle, read the log. It is loud
  about missing DXC, because that is the failure that otherwise surfaces much
  later as a shader refused for no visible reason. `Show refusals only` filters
  the log to the REFUSED and NOTE lines, deduplicated, which is the part of a
  long log anybody actually needs.
  `dxr-tier-11.example.ini` is the settings file to copy and rename.
  `build_phase4.bat` builds the Tier 1.1 probe into `phase4out\`, a directory
  with no proxy in it so the probe measures the real runtime; copy `d3d12.dll`
  in to measure the shim instead. `tier11probe.exe [warp|hw]` runs the three
  feature probes, and takes `-debug` for the debug layer, `-gfxsplit` for
  graphics state across a split, `-batchsplit` for dispatch batching and its
  control, `-time` and `-pipeline` for the split cost, and `-gpuinst` to put
  the TLAS instance descriptions in GPU-only memory so the shim has to copy
  them out instead of mapping them.
  `build_rewriter.bat` builds the C++ rewriter and `phase5out\dxrw.exe`,
  copying DXC beside it. `dxrw lower` works on `.ll`, `dxrw rewrite` takes a
  container and returns a signed one, which is the path the proxy uses.
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
| Loop body reading caller locals that do not fit the payload | The any-hit shader is a separate invocation; the payload is the only shared state |
| Wave intrinsics around the query | Promotion to raygen changes lane occupancy |

Two rows have LEFT this table, and both are worth understanding, because each
was written down as impossible and neither was.

**`CommittedGeometryIndex`.** MEASURED and true: `GeometryIndex()` in a DXR 1.0
hit shader is ITSELF Tier 1.1, sets shader flag 0x2000000, and
`CreateStateObject` returns `E_INVALIDARG` on the 1070. The note here said a
route existed in principle, encoding the index in the shader table, "but that
means the shim rebuilding the application's SBT. Not attempted." The shim now
BUILDS the table rather than rebuilding one, so the objection dissolved. The
same mechanism took `*InstanceContributionToHitGroupIndex` with it.

**Multiple concurrent RayQuery objects.** Still refused, but the reason given
here, one payload and one in-flight trace, only applies to queries that are
genuinely LIVE at once. The check counts allocations, and Unreal's are
sequential. See the entry below.

The lesson both share: an impossibility claim is only as durable as the
assumption under it, and neither assumption was written down beside the claim.

The pixel-shader case is the most consequential of what remains, since it is
legal in DXR 1.1 and some engines use it.

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

**The tier flip was opt-in for a long time and is now the default**, once the
rewriting path it promises actually existed and was gated on DXC being present.
See the position section for why that changed and what it is conditional on.

The principle that drove the old gate has NOT changed and still governs:
reporting 1.1 entitles an application to emit RayQuery, and a shim that claims
1.1 and then fails is worse than one that claims 1.0. What changed is where the
check lives. It used to be an environment variable the user had to set; it is
now a check on whether the shim can actually honour the claim.

A shader the rewriter refuses is still logged and forwarded unchanged, so the
application gets the driver's own error rather than a silently wrong render.

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

**Result: WORKING (2026-09-21).** A RayQuery compute shader, unmodified, runs
on the GTX 1070 and matches WARP bit-exactly. Four shaders covering both
patterns, 0 mismatches. `.\tools\run_dispatch_test.ps1`. The dispatch path is
`proxy/rq_pipeline.{h,cpp}`; see the position section for how it works and what
is still untested.

**DXC host (2026-09-21).** `proxy/rewriter/dxc_host.{h,cpp}` converts a
container to text and back and signs the result, loading DXC by full path from
beside the shim. The rewriter and its host are linked into the proxy. The
dispatch path is the one structural piece still missing, see the position
section.

**Proxy detection (2026-09-21).** `proxy/dxil_scan.{h,cpp}` detects RayQuery
from SFI0 bit 20, no bitcode parsing needed, hooked at the two pipeline
creation paths and at `CreateStateObject`. Forwards unchanged; the log is the
clarity. The transform itself is a costed fork, see the position section.

**Rewriter result (2026-09-21).** `phase5/rewriter/` automates the transform
and reproduces both hand lowerings bit-exactly against WARP, 14450 and 8117 of
65536 rays, 0 mismatches. It refuses eight distinct shapes in the analysis and
one more in the lowering, each provoked by a test. Opcodes are a whitelist; unknown ones stop the lowering
rather than being guessed at.

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

`.\tools\run_proxy_test.ps1` copies DXC beside the proxy, and must. Without
it the shim stands aside and the comparison is the unmodified path against
itself, which passes and means nothing. It warns if DXC is not there.

Phase 5 has its own end-to-end check, `.\tools\run_rewriter_test.ps1`:
lower each pattern with the rewriter, assemble and sign with `dxilrt`, run on
the 1070, diff against WARP. It also runs the refusal tests, because an
analysis that never says no is worth as little as one that never finds
anything.

`raytest.exe` carries the Phase 5 hooks:
- `--lib <file.dxil>` makes the TraceRay side load a pre-built library from
  disk instead of compiling HLSL, so anything the rewriter produces can be held
  against the same ground truth.
- `--roundtrip` sends every shader out to `.ll` text and back before D3D12 sees
  it. `--rtpoison` does the same but corrupts the lowered side on purpose, and
  must DIVERGE.
- `--contrib`, with `--multi` or `--mixed`, gives the two instances different
  `InstanceContributionToHitGroupIndex` values, so the shader table has to be
  sized to the scene rather than to one record.
- `--mixed` builds one triangle instance and one procedural instance under one
  top-level structure, the AABB being the same unit box `--proc` uses so the
  existing test shaders work on it unchanged. With `--contrib` the two kinds
  land on different hit group records, which is the case the typed table
  serves; without it they share record 0, which is the case nothing can serve.
- `--both` makes the harness build TWO hit groups and a two-record hit table,
  for a library that commits both kinds. Only the `--lib` path needs it; the
  proxy builds its own.
- `--table` binds a four-entry UAV descriptor table with the real output at
  slot 2 and decoys at 0, 1 and 3, so a shader that resolves the wrong index
  writes nowhere visible. **Both** sides honour it now; until dynamic indexing
  was added the RayQuery side did not, which quietly kept every array case out
  of the proxy's dispatch path.
- `DXR_TIER11_DEBUGLAYER=1` turns the D3D12 debug layer on in a release build AND
  drains the InfoQueue. Both halves are needed: the layer reports through
  `OutputDebugString`, so without the drain a console sees nothing and its
  silence proves nothing. **The harness reads this, the SHIM does not.** For a
  real application use `dxcpl.exe`, the DirectX Control Panel, which forces the
  layer on for any executable you name.

The shim reads six SHIPPING settings. It also reads the bisect knobs `nowrap`, `rqstub`, `rqphase`, `rqlimit`, `rqonly` and `dump`, which are diagnostics for the GPU crash and are not part of the product. Four are `d3d12.dll`'s
and two are `dxgi.dll`'s. `tier11` defaults
ON, `nowrap` defaults off and `log` defaults to `%TEMP%`, each settable in
`dxr-tier-11.ini` beside the DLL or as `DXR_TIER11` / `DXR_TIER11_NOWRAP` /
`DXR_TIER11_LOG` in the environment, which wins. `nowrap` is not a companion to
`tier11`, it OVERRIDES it: everything the shim does lives on the device object
it hands the application, the Tier 1.1 answer included, so declining to hand
that object over switches the whole layer off.

`spoof` defaults ON and `spoofid` defaults to 0x1F08, both read only by
`dxgi.dll`, which does nothing at all unless that file was copied in.

`log` takes a file or a FOLDER, resolves a relative path against the SHIM's
directory rather than the process working directory, and falls back to `%TEMP%`
with a complaint on the first line if it cannot open what it was given. It is
the one setting read from `DllMain`, because the start marker is written there
and has to know where to go; see the exception written into proxy/config.h.

Check sensitivity before believing a pass, and check the CHECK. Five times in
this project a test passed for the wrong reason:
- the `-gfxsplit` control re-bound a PSO the test had abandoned;
- the first `--rtpoison` corrupted BOTH sides, so they still agreed;
- the descriptor-array poison substituted on `@outBufs` when the rewriter
  synthesises `@rq_uav0`, so it never applied and the "failure" case was
  byte-identical to the passing one;
- **the debug layer's silence was read as evidence when it was not even
  running.** `raytest` enabled the layer only in a `_DEBUG` build, and the
  layer reports through `OutputDebugString`, which a console never sees. The
  fix was `DXR_TIER11_DEBUGLAYER=1` plus an `ID3D12InfoQueue` drain, as the Phase 4
  probe already does. The control that exposed it: run the layer against a case
  ALREADY KNOWN to be wrong. It stayed silent there too, so the silence meant
  nothing either way.

- **a CORRECT change to the product silently emptied a test.** Making the shim
  stand aside when DXC is absent was right. But `run_proxy_test.ps1` exists to
  prove the WRAPPER is transparent, and the Microsoft sample folders contain no
  DXC, so the shim stood aside there and the comparison became the unmodified
  path against itself. It would have passed forever while proving nothing. The
  script now copies DXC beside the proxy and warns loudly if it cannot.

The debug layer one generalises: **an oracle that says nothing has to be shown
capable of saying something**, on the same run, before its silence counts as a
result.

The last one generalises differently, and is the harder lesson: **when you
change what the product DOES, re-ask what each test is still measuring.** That
test did not break, did not warn, and did not fail. It quietly changed subject.
Nothing in a green suite tells you this happened.

And one failure that was not a test at all, but the same shape of mistake. A
CRLF `.ll` made the C++ rewriter refuse with a message about the query handle,
which sent an hour after a phantom bug in a fresh port. **Run the OLD code
through the NEW path before believing the new code is wrong.** It failed there
too, which located the fault in the path immediately.

A test that cannot fail has not been run. When a sensitivity check reports the
same result as the real run, suspect the check first: diff what it actually
produced before concluding anything about the code.

## Working method

Read the source before designing. In the investigation that produced this
brief, every architecture reasoned out in the abstract turned out to be wrong,
and every correct finding came from reading actual files. Prefer a grep over a
hypothesis.

State clearly what is verified versus inferred. Flag uncertainty rather than
guessing.

**Byte-identity has TWO blind spots, both found the hard way.** It proves the
two implementations AGREE, not that either is right: they shared the phi
blindness identically. And it compares OUTPUTS, so it cannot see a missing
REFUSAL, which is how the C++ silently lost the Python's "loop body branches
outside the loop" check and emitted identical bytes for everything tested while
disagreeing about what to reject.

**When porting, require identical OUTPUT, not passing tests.** Both
implementations get checked against the same cases, so a shared
misunderstanding passes twice. The C++ rewriter had to produce byte-identical
`.ll` to the Python, and that check failed on its first run and found a bug in
the ORIGINAL. Also: do not reproduce the original's accidents in a second
language, fix them in both.

**Test with something you did not write for the purpose.** Every Phase 5 result
up to the independent shader came from two shaders written to demonstrate one
lowering, and they shared an assumption none of them could reveal: that a
Proceed loop never touches a resource. The first genuinely independent shader
found it immediately. A suite that only contains cases built to pass is
measuring itself.

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
