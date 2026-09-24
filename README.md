# dxr11-pascal

A user-mode D3D12 layer that makes GPUs reporting `D3D12_RAYTRACING_TIER_1_0`
present as Tier 1.1, by translating the Tier 1.1 features into DXR 1.0
operations the driver already supports.

Target hardware is NVIDIA Pascal (GTX 10-series) and Turing GTX 16-series.
Both report Tier 1.0. Development and every measurement in this repository are
on a GTX 1070.

**A RayQuery compute shader, unmodified, runs on a GTX 1070 and produces
bit-exact output against WARP.**

![How the shim sits between an application and the real D3D12 runtime](docs/shim-overview.svg)

The purple layer is this entire project. Everything else already exists on the
machine, and the GPU does all of the actual ray tracing.

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
| Indirect dispatch of a `RayQuery` compute shader | The same split. The group counts are captured by replaying the application's own `ExecuteIndirect` with a tiny shader of the shim's |
| Inline ray tracing (`RayQuery`) | The DXIL is rewritten into a DXR 1.0 library, with generated any-hit, intersection, closest-hit and miss shaders |

There is a second, separate proxy, `dxgi.dll`, which is not a Tier 1.1 feature
at all. Unreal Engine refuses ray tracing on Pascal by PCI **device id**, after
it has already accepted the tier, with no setting that overrides it. So
`dxgi.dll` reports the card under a Turing device id and changes nothing else:
not the vendor id, not the adapter description, and no capability answer. It is
a separate file so it can be left out, but for Unreal it is not optional.

The fourth is the real work. A `RayQuery` compute shader is lowered so that the
`Proceed()` loop body becomes an any-hit shader, or an intersection shader, or
both, and the committed accessors travel in the ray payload.

## Scope

**Every ray tracing feature an application uses is emulated; nothing is
switched off.** Where the shim does not yet handle something, the stopgap is
named in the log and in the release notes: a refused shader gets a pipeline
that does nothing, and that pass draws nothing. A capability is reported
unsupported only where the application's own fallback draws the same image,
shown from its source.

Done: RayQuery (all 25 accessors Unreal uses), indirect `DispatchRays`,
`AddToStateObject`, the new ray flags, and every RayQuery shader in the Unreal
game used for testing. Next, in order:

1. the rest of Tier 1.1, which the shim already reports: `GeometryIndex()` in
   an application's own ray tracing shaders, the pipeline-wide skip flags, the
   rewriter's remaining refusals, and `RayQuery` inside ray tracing shaders,
   beside `groupshared` memory or wave operations, and in pixel, vertex, mesh
   and amplification shaders
2. NVIDIA cluster operations (RTX Mega Geometry), which Unreal uses to ray
   trace Nanite at full detail
3. Tier 1.2: Shader Execution Reordering, fully, measured faster on the GTX
   1070 where shading diverges (see below); opacity micromaps, NVAPI's form and
   DXR 1.2's, where a 2-state micromap decides opacity without running the
   any-hit shader, so emulating it means reproducing the micromap lookup; and
   Shader Model 6.9 with DXR 1.2 in full, claimed only once SER is complete
4. spheres and linear swept spheres
5. DLSS, including Ray Reconstruction, last. It needs tensor cores, which
   Pascal lacks; how to emulate it, and whether NVIDIA's license allows running
   its networks another way, are open

**Shader Execution Reordering: full SER or none.** Read from Unreal's source: Lumen hit lighting, ray traced
translucency, the path tracer and NvRTX's RTXDI passes each choose between an
SER and a non-SER version of the same pass, from what NVIDIA's driver reports
through NVAPI. The shim passes that question through, and the driver says no
on Pascal. The SER version traces, regroups threads, then runs the hit shader;
the other makes one `TraceRay` call with the same ray and payload. Same hit,
same shader, same image. Reordering only changes which threads run side by
side. So today nothing is lost. The shim will not tell an application SER
works until all of it is emulated and verified, because a half-done SER would
move Unreal onto a path that could lose ray tracing. The reordering itself
was measured on the GTX 1070 with a software version (trace, spill, sort by
material, then shade), `bench/sertest.cpp`, see
[docs/ser-test.md](docs/ser-test.md). Where materials are mixed it is 1.3x to
18x faster than plain `TraceRay`, the more so the heavier the shading, because
Pascal runs divergent hit shaders one after another. Where they are already
grouped it is up to 2x slower. So it is worth building, applied where shading
diverges.

## Status

Verified on a GTX 1070, with WARP as the oracle for every result:

- 36 end-to-end render cases, all bit-exact, plus 4 refusal gates and a
  sensitivity gate
- 14 rewriter cases, each byte-identical between the Python reference and the
  C++ port, on both the `.ll` path and the DXIL container path, plus 45
  analysis and lowering checks, 3 record-read checks, an append check and a
  driver check of NVAPI calls with the extension slot registered
- Two Microsoft DXR 1.0 samples run through the proxy unchanged, one of them
  pixel-identical to its no-proxy baseline

Shaders taken from a shipping Unreal Engine 5.8.2 game, which nobody here
wrote, go DXIL container in and signed container out, and the state objects
built from them survive on a GTX 1070. That last part is new in 0.37.0: the
game's GPU crash during pipeline creation was traced to this shim, and fixed.
**A full run of an Unreal game does not work yet**, mainly because Unreal
treats a shader the shim refuses as fatal. See
[Notes for Unreal](docs/usage.md#9-notes-for-unreal) for where it stands.

A lowered `RayQuery` compute pipeline dispatched INDIRECTLY, through
`ExecuteIndirect` with a `DISPATCH` signature, is emulated since 0.39.0.
Unreal dispatches many of its Lumen and MegaLights passes this way, and until
then those passes silently drew nothing. The group counts are read back from
the GPU without touching the state of the application's argument buffer.

Tier 1.1 is reported by default. A proxy DLL only sits beside an executable
because somebody put it there, so the install is the opt-in, and asking for a
second one through an environment variable mostly produced reports that the
shim does nothing.

Turning it off is `tier11 = 0` in a `dxr-tier-11.ini` beside the DLL. Uninstalling is
deleting the DLL. Both are one step, and they are different things: the first
leaves the shim loaded and forwarding, the second leaves no trace at all.

## Using it

Build the proxies, then put the resulting DLLs next to the target executable.
That is the whole installation:

```
build_proxy.bat
build_dxgi.bat
copy d3d12.dll <target directory>
copy dxgi.dll <target directory>
```

`dxgi.dll` is only needed for an application that refuses Pascal by device id,
which in practice means Unreal. It does nothing unless it is copied in.

For a `RayQuery` shader to be rewritten, `dxcompiler.dll` and `dxil.dll` have
to be in that directory too. They load lazily, so a plain DXR 1.0 application
never needs them.

Settings, if you want any, go in a `dxr-tier-11.ini` beside the DLL. See
[dxr-tier-11.example.ini](dxr-tier-11.example.ini). A file is used rather than environment
variables because a game started from Steam or the Epic launcher never sees a
variable you set in a console.

The log lands in `%TEMP%\dxr-tier-11-proxy.log` and says what was intercepted, what
was lowered and what was refused, with reasons.

**[docs/usage.md](docs/usage.md) is the step by step guide**, including where
the DLL has to go for Unreal, which files go beside it, how to read the log,
and what to try when something goes wrong.

## What it refuses, and why

A shader the rewriter cannot lower is logged and forwarded unchanged, so the
application gets the driver's own error rather than a silently wrong render.
Nothing here is guessed at: each refusal is provoked by a test.

Refused because DXR 1.0 offers nothing to lower onto. Under the scope above
the first three are still to be solved, as the end of Tier 1.1, not accepted:

- `RayQuery` in a pixel, vertex, mesh or amplification shader. `DispatchRays`
  only launches a raygen, and there is no promotion path
- `RayQuery` inside an any-hit or intersection shader, neither of which can
  call `TraceRay`
- `groupshared` memory, group barriers or wave intrinsics in the same entry
  point as the query
- a loop body reading caller locals that do not fit the payload. From 0.42.0
  a value read before the loop is carried: re-read in the hit shader when it
  comes from a read-only resource, and otherwise stored in the payload, up to
  16 i32, float or i1 values. Still refused: more than that, a value computed
  after `TraceRayInline`, and a loop body that becomes an intersection shader,
  which has no payload
- a loop body with a side effect other than an APPEND, such as a UAV write at
  a fixed index or an atomic. The any-hit shader it would become runs in no
  defined order, so the write would not be the same write. An append, a
  counter update and stores at the index it returned, IS lowered, from
  0.41.0: its result does not depend on the order, and the shim sets
  `NO_DUPLICATE_ANYHIT_INVOCATION` on every bottom-level geometry so the
  any-hit runs once per hit, as the loop did. An append is still refused
  alongside `Abort()`, or where the loop body becomes an intersection shader,
  which may run more than once whatever the flags say
- an application routing both triangle and procedural geometry to the same hit
  group record, which no shader table can serve

Refused because the rewriter has not been taught them yet, which is a different
thing and is said differently in the log:

- several `RayQuery` objects in one entry point when one of them commits
  procedural hits. Several queries share one generated hit group and pick
  their loop body from a query id in the payload, from 0.43.0; an
  intersection shader has no payload, so it cannot tell them apart. A query
  traced INSIDE another's Proceed loop is refused too, and that one is a fact
  about DXR 1.0: the loop becomes an any-hit shader, which cannot call
  `TraceRay`
- a Shader Model 6.6 binding into a resource array at a dynamic index. The 6.5
  form of exactly that works
- an NVAPI shader extension call other than the RayQuery cluster ID ones.
  NVIDIA's HLSL extensions are stores to a `RWStructuredBuffer` of
  `NvShaderExtnStruct`, which the driver reads as intrinsics while the
  application has registered that buffer's slot, as Unreal does around its
  compute pipelines. The candidate and committed cluster ID calls (ops 94 and
  95) are replaced with 0xFFFFFFFF, the answer for every geometry on a card
  without cluster operations, which is what Pascal reports; any other call is
  refused rather than moved into a DXR 1.0 shader

**All 25 of the `RayQuery` accessors Unreal uses are now supported.** Four of
them, `Candidate`/`CommittedGeometryIndex` and the two
`*InstanceContributionToHitGroupIndex` forms, were listed here as permanently
impossible, and they were, for as long as the application owned the shader
table. This shim builds it, so it knows those numbers for every record it
writes. Each hit group record carries a pointer, a local root signature root
SRV, to its own (geometry, contribution) pair, and the hit shader reads it.
The first version of this made the Pascal driver crash inside
`CreateStateObject`; 0.37.0 to 0.39.x worked around that by compiling a copy
of the hit shaders per pair, which cannot scale to a real open world. 0.40.0
found the actual cause, a Shader Model 6.6 resource handle left unannotated,
and went back to reading the record.

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

## Versions

[CHANGELOG.md](CHANGELOG.md) carries the releases and the versioning policy.
The version is compiled into the DLL, put in its version resource, and logged
at the start of every run, so both the file and any log identify their own
build:

    2026-09-22 10:20:00.499 [dxr-tier-11-proxy-log] ======== start: MyGame.exe (pid 4872), shim 0.15.0 ========

The log also reports the versions of the four DLLs that are not ours and that
decide whether a run works: `dxcompiler.dll` and `dxil.dll`, which convert and
sign the rewritten shader, and the application's own `D3D12Core.dll` and
`d3d12SDKLayers.dll`. Full paths, because the same file name arrives from
System32, from beside the exe, or from an Agility subdirectory.

## Documentation

`docs/` carries the reasoning and the measurements behind each phase, including
the designs that turned out to be wrong. `CLAUDE.md` is the working brief.

## Disclaimer

Not affiliated with, endorsed by or supported by NVIDIA or Microsoft. This
project redistributes no code belonging to either. It is a compatibility layer
built against public APIs.
