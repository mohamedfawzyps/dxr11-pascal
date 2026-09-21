# Phase 3: proxy d3d12.dll - notes

Goal: a proxy `d3d12.dll` that an app loads instead of the system one, forwards
everything to the real runtime, and (later) wraps the device so we can spoof the
raytracing tier and intercept calls. No translation in this phase.

## Why a proxy DLL works

Windows searches the exe's own directory before System32 for a DLL loaded by
name. Dropping our `d3d12.dll` beside an app's exe makes the app bind to us. We
load the real runtime by full System32 path, so forwarding never recurses.

## Staged plan

- 3a: forwarding-only. Exports the d3d12 entry points, forwards each to the real
  d3d12.dll, and logs D3D12CreateDevice to confirm the app runs through us.
  Fully transparent, no device wrapping. Source: `proxy/d3d12_proxy.cpp`,
  exports in `proxy/d3d12_proxy.def`. **PASSED, see Result below.**
- 3b: wrap the returned `ID3D12Device5`, forwarding every method unchanged, and
  re-verify. This is the seat for Phase 4/5 (tier spoof, CreateStateObject,
  command-list interception). **PASSED, see Result: 3b below.**

## Exports must match the real DLL's ORDINALS, not just its names

This was the one real finding of 3a, and it is not obvious.

The Windows SDK's `d3d12.lib` imports **`D3D12CreateDevice` by ordinal 101**,
not by name. `dumpbin /imports` on any app linked against it shows:

    d3d12.dll
                       Ordinal   101
                     E D3D12SerializeRootSignature

So a proxy that exports all the right *names* but lets the linker assign
ordinals 1..N fails at load time with `STATUS_ORDINAL_NOT_FOUND`
(`0xC0000138`), before any of our code runs. The first build did exactly that.

The real table (Win11 26100, `dumpbin /exports C:\Windows\System32\d3d12.dll`),
ordinal base 99:

| Ord | Name | Ord | Name |
|---|---|---|---|
| 99 | *[NONAME]* | 109 | D3D12EnableExperimentalFeatures |
| 100 | SetAppCompatStringPointer | 110 | D3D12GetInterface |
| 101 | D3D12CreateDevice | 111 | D3D12PIXEventsReplaceBlock |
| 102 | D3D12GetDebugInterface | 112 | D3D12PIXGetThreadInfo |
| 103 | D3D12CoreCreateLayeredDevice | 113 | D3D12PIXNotifyWakeFromFenceSignal |
| 104 | D3D12CoreGetLayeredDeviceSize | 114 | D3D12PIXReportCounter |
| 105 | D3D12CoreRegisterLayers | 115 | D3D12SerializeRootSignature |
| 106 | D3D12CreateRootSignatureDeserializer | 116 | D3D12SerializeVersionedRootSignature |
| 107 | D3D12CreateVersionedRootSignatureDeserializer | 117 | GetBehaviorValue |
| 108 | D3D12DeviceRemovedExtendedData | | |

`proxy/d3d12_proxy.def` now pins each implemented export to its real ordinal.

**Verified:** the 8 publicly documented entry points are enough for both test
apps. **Not verified:** whether the internal ones (layering, PIX, appcompat,
NONAME 99) are ever needed. They are left unexported deliberately, so that an
app that wants one fails by name rather than silently misbehaving. Note they
cannot be added as .def forwarders, because a forwarder names a module and
`d3d12` would resolve back to this proxy in the app directory; they would need
either asm tail-jump thunks or known signatures.

## Build (Windows x64)

    build_proxy.bat                                       -> d3d12.dll
    build_sample.bat                                      -> HelloWorld
    build_sample.bat D3D12RaytracingSimpleLighting        -> SimpleLighting
    build_sample.bat D3D12RaytracingSimpleLighting debug  -> debug layer on

Samples land in `sampletest\<SampleName>\`. Samples root comes from
`%DXSAMPLES_DIR%`, default `C:\DW\DirectX-Graphics-Samples`.

Then run either one through the proxy with:

    tools\run_proxy_test.ps1 -Exe sampletest\<Name>\<Name>.exe [-Animated]

which runs it twice, once clean and once with our d3d12.dll dropped beside it,
and reports survival, fps, the proxy log, and (for a static scene) the closest
matching frame pair.

## Result: 3a PASSED (2026-09-21)

Two independent DXR 1.0 apps, both on the GTX 1070, both through the proxy.

**1. `raytest.exe`, the Phase 2 harness.** Headless, builds a BLAS and TLAS,
creates a state object and shader table, and runs `DispatchRays` on both WARP
and the 1070. Through the proxy it still produces the Phase 2 result exactly:

    opaque: 65536 rays, 14450 hits, 0 mismatches, max |dt| 0.000000
    alpha : 65536 rays,  8117 hits, 0 mismatches, max |dt| 0.000000
    OVERALL: ALL MATCH

The log caught all four device creations (WARP and hardware, twice):

    [dxr-tier-11-proxy-log] attached to process
    [dxr-tier-11-proxy-log] D3D12CreateDevice fl=0xc000 hr=0x00000000 device=00000268EECA6E90
    ... x4

**2. `D3D12RaytracingHelloWorld`, the Microsoft DXR 1.0 sample.** Windowed,
swapchain, continuous frames, Agility SDK runtime. Ran 8 seconds with and
without the proxy and the window was captured at t=5s in both runs:

| | baseline | through proxy |
|---|---|---|
| fps | ~2140 | ~2140 |
| Mrays/s | ~1970 | ~1970 |
| frame | 1280x720, 14813 distinct colours | identical |
| pixel diff | - | **0 differing pixels, max channel delta 0** |

Proxy log from the sample run:

    [dxr-tier-11-proxy-log] attached to process
    [dxr-tier-11-proxy-log] D3D12CreateDevice fl=0xb000 hr=0x00000001 device=0000000000000000
    [dxr-tier-11-proxy-log] D3D12CreateDevice fl=0xb000 hr=0x00000000 device=0000021FF78F41B0
    [dxr-tier-11-proxy-log] D3D12CreateDevice fl=0xb000 hr=0x00000000 device=0000021FF7D02E10

The first line is `hr = S_FALSE` with a null device: that is the documented
capability-probe form of `D3D12CreateDevice` (null `ppDevice`), used by
`DeviceResources` to test feature level support. Not an error.

**3. `D3D12RaytracingSimpleLighting`, the second Microsoft sample.** Added
because it exercises more device API surface per frame than HelloWorld: a
3-descriptor heap rather than 1, a per-frame constant buffer, and index and
vertex buffers with normals, plus a rotating camera and light. Through the
proxy it runs identically:

| | baseline | through proxy |
|---|---|---|
| survived 8s | yes | yes |
| fps | ~1377 | ~1383 |
| device creations logged | - | 3, same shape as HelloWorld |

No frame comparison is claimed for this one, and the harness refuses to make
one (`-Animated`). See the caveat below; it is a limitation of our capture, not
a difference between the runs.

The proxy is transparent on both. Proceed to 3b.

### Caveat: our window capture is not an oracle for animated samples

Worth writing down so it is not rediscovered later.

**Measured:** on SimpleLighting, window grabs taken seconds apart *within a
single run* come back byte-identical, even though the app reports ~1380 fps and
its `OnUpdate` rotates the camera and light every frame. Across two runs, a
constant 9609 of 14400 sampled pixels differ, and that number is the same at
every timestep.

**Not verified:** why. Two candidates, not distinguished. Either `BitBlt` on the
window DC returns a stale frame for these flip-model swapchains, or the sample's
per-frame re-rotation of `m_eye` settles into a fixed view. An earlier guess
that accumulated float drift explained it was not supported by the measurement
and has been dropped.

Either way the capture path cannot be trusted to sample live frames here, so we
do not draw conclusions from it for animated content. Two other capture
approaches were tried and also fail on a flip-model swapchain: `PrintWindow`,
and `BitBlt` while the window is occluded, both return near-blank images.
Forcing the window topmost and foreground before each grab is what makes the
static case work at all. If a later phase needs real frame comparison, it wants
a proper path, a hooked `Present` or a UAV readback, not window grabbing.

None of this weakens the 3a result: the pixel-exact claim rests on HelloWorld,
whose scene is static, and on raytest.exe, which compares buffer contents
directly rather than pixels.

## Result: 3b PASSED (2026-09-21)

`proxy/d3d12_device.h` / `.cpp` define `Dxr11Device`, which implements
`ID3D12Device5` and forwards all 62 methods unchanged. `D3D12CreateDevice` now
wraps the real device and hands back the wrapper. `DXR11_NO_WRAP=1` turns
wrapping off and restores exact 3a behaviour, which is useful for bisecting.

Signatures were transcribed from the Agility 1.619.5 `d3d12.h` on this machine,
not from memory, and the proxy now builds against those same headers. Building
at `/W4` with every method marked `override` is itself a check: a wrong or
missing signature leaves the class abstract and the build fails.

Re-verified with the full 3a suite, all three still green:

| | result |
|---|---|
| `raytest.exe` | ALL MATCH, both patterns still bit-exact, all 4 devices wrapped |
| HelloWorld | **0 of 14400 pixels differ**, ~2231 vs ~2214 fps |
| SimpleLighting | runs, ~1416 vs ~1415 fps |

Refcounting is clean: every `device wrapper destroyed` line pairs with its
creation on the runs that shut down normally.

### What the wrapper deliberately does NOT do

Only the device is wrapped. Child objects (queues, lists, resources, heaps) come
back exactly as the runtime made them. Two consequences:

1. `ID3D12DeviceChild::GetDevice()` on any child returns the **real** device, so
   an app reaching the device that way bypasses us entirely. Harmless while we
   only forward; Phase 4 has to deal with it.
2. Nothing needs unwrapping on the way in, because the app never holds a wrapped
   child to hand back to us.

`QueryInterface` answers for `IUnknown`, `ID3D12Object` and `ID3D12Device`
through `ID3D12Device7`. Anything else is passed to the real device **and
logged**, so we find out what apps actually ask for instead of guessing. The
samples produce exactly one such line:

    [dxr-tier-11-proxy-log] device QI PASSED THROUGH UNWRAPPED: ID3D12InfoQueue

which is correct: the debug layer wants the real InfoQueue.

### Extended to ID3D12Device7 (2026-09-21)

Originally the wrapper stopped at Device5 and passed Device6 and above through
unwrapped. The Phase 4 probe closed that question: `ID3D12Device7` **is present
on the GTX 1070** even though it reports Tier 1.0, so `AddToStateObject` is
reachable on the target hardware and an unwrapped Device7 would let an app
bypass the shim entirely. See docs/phase4-probe.md.

The wrapper now covers Device6 (`SetBackgroundProcessingMode`) and Device7
(`AddToStateObject`, `CreateProtectedResourceSession1`), 65 methods in total.
Both are optional: the constructor queries for them and `QueryInterface` returns
`E_NOINTERFACE` for an IID the real device does not offer, rather than handing
back a vtable the device cannot honour.

Verified by running the Phase 4 probe through the proxy. On WARP the call lands
in our wrapper and succeeds:

    [dxr-tier-11-proxy-log] device wrapper created (real=..., Device6=yes, Device7=yes)
    [dxr-tier-11-proxy-log] AddToStateObject additions=4 grow-from=... hr=0x00000000

with the probe still reporting identical results, and no
`QI PASSED THROUGH UNWRAPPED` line anywhere. On the 1070 the wrapper also
reports `Device7=yes`, confirming the probe finding from inside the shim.

Regression after the change: raytest ALL MATCH, HelloWorld 0 of 14400 pixels
differ (2162 vs 2160 fps), SimpleLighting unchanged (1385 vs 1384 fps), and the
debug build through the wrapper produces 3 debug-layer lines with 0 errors, the
same set as the no-proxy baseline.

Device8 and above are still passed through unwrapped and logged. Extend when the
log shows an app asking for one.

## The debug layer needs three undocumented exports

This was the real find of 3b, and it is a **3a bug**, not a wrapper regression:
`DXR11_NO_WRAP=1`, which is forward-only 3a behaviour, reproduces it.

With the proxy in place the D3D12 debug layer silently did nothing. No adapter
line, no state object dump, no messages at all, while the same binary without
the proxy produced all of them. What surfaced the cause was:

    D3D Error 887e0003: D3D12 SDKLayers dll not found at D3D12SDKPath.

That message is misleading. The DLL is exactly where it should be. The real
cause is that `d3d12SDKLayers.dll` imports three functions from `d3d12.dll`
**by name**:

    dumpbin /imports d3d12SDKLayers.dll
      d3d12.dll
        D3D12CoreGetLayeredDeviceSize
        D3D12CoreRegisterLayers
        D3D12CoreCreateLayeredDevice

Our proxy is the module named `d3d12.dll` in the app directory, so those imports
bind to us. We did not export them, the load failed, and D3D12Core reported the
failure as a missing file.

They are undocumented, so we cannot write C forwarders without guessing their
signatures. `proxy/d3d12_thunks.asm` forwards them as x64 tail jumps instead,
which need no signature at all: the arguments are already in the right registers
and on the caller's stack, and the real function returns straight to our caller.
Each thunk puts an index in `r10` and jumps to a common helper that resolves the
target through `Dxr11ResolveThunk` and tail-jumps to it. The helper always
spills and restores the argument registers, even once resolved, so that it has
one ordinary prologue that x64 unwind info can describe.

With that in place the debug layer is fully back, and it now validates the
wrapper for us:

    [dxr-tier-11-proxy-log] thunk D3D12CoreRegisterLayers -> 00007FF82676F190
    [dxr-tier-11-proxy-log] thunk D3D12CoreGetLayeredDeviceSize -> 00007FF826761E90
    [dxr-tier-11-proxy-log] thunk D3D12CoreCreateLayeredDevice -> 00007FF8267622E0
    Direct3D Adapter (0): VID:10DE, PID:1B81 - NVIDIA GeForce GTX 1070
    | D3D12 State Object ...: Raytracing Pipeline
    D3D12 WARNING: ... CREATERESOURCE_STATE_IGNORED

That is the identical message set the no-proxy baseline produces, including the
same single pre-existing warning, and nothing about refcounts or interfaces.
The debug build arms `SetBreakOnSeverity(ERROR)`, so any debug-layer error would
have killed the process; it survived.

**A misleading observation, recorded so it is not repeated.** Enumerating the
process's loaded modules showed `d3d12SDKLayers.dll` present even while the
debug layer was broken, which argued against this diagnosis and cost time.
Do not use the module list to decide whether a DLL's imports resolved; a failed
load can still appear there. The A/B that actually settled it was implementing
the exports and watching the debug layer come back.

Still unexported, and so far never observed to be needed: the PIX entry points
(111-114), `D3D12DeviceRemovedExtendedData` (108), `GetBehaviorValue` (117),
`SetAppCompatStringPointer` (100), and the NONAME at 99. Adding one is now three
lines: a `THUNK` line in the `.asm`, a name in `g_thunkNames`, an ordinal in the
`.def`.

## Building the MS sample without NuGet

`build_sample.bat` compiles the sample straight out of the
DirectX-Graphics-Samples checkout with `cl.exe`, leaving that repo untouched.
The upstream `.vcxproj` needs two NuGet packages and `nuget.exe` is not
installed here; the sample has no PIX *code*, only the msbuild import, so PIX is
skipped and the Agility SDK already at `C:\DW\microsoft.direct3d.d3d12.1.619.5`
is used. The `FxCompile` step is reproduced by hand as
`dxc -T lib_6_3 -Vn g_pRaytracing -Fh CompiledShaders\Raytracing.hlsl.h`.

### Upstream bug in these samples, patched at build time

`D3D12RaytracingHelloWorld::m_descriptorsAllocated`
(`D3D12RaytracingHelloWorld.h:65`) is never initialised by the constructor. It
is only zeroed in `ReleaseDeviceDependentResources()`, which does not run before
the first `CreateDeviceDependentResources()`. So `AllocateDescriptor()` reads an
uninitialised member on the first init and computes a garbage descriptor index:

    D3D12 ERROR: ID3D12Device::CreateUnorderedAccessView: Specified CPU
    descriptor handle ptr=0x000000058F3E5B82 does not refer to a location in a
    descriptor heap. [ EXECUTION ERROR #646: INVALID_DESCRIPTOR_HANDLE ]

followed by a ~5 second stall and an access violation (`0xC0000005`). The sample
object is a local in `WinMain`, so whether this bites depends on stack contents;
it reproduced every time with this toolchain, and it reproduces **without the
proxy present**, so it is not ours. `tools\patch_sample.ps1` adds
`m_descriptorsAllocated(0)` to the constructor init list of a generated copy of
the `.cpp`, and fails loudly if the anchor text moves. It also detects the case
where upstream has since fixed it and copies through unchanged.

**Both samples carry it**, so it is a shared-template bug rather than a quirk of
one sample:

| Sample | declared | zeroed only in Release... | read in AllocateDescriptor |
|---|---|---|---|
| HelloWorld | `.h:65` | `.cpp:568` | `.cpp:677` |
| SimpleLighting | `.h:77` | `.cpp:736` | `.cpp:848` |

Worth remembering for later phases: a garbage descriptor handle on Pascal
presents as a GPU hang plus an AV, not a clean error return. The D3D12 debug
layer named it immediately. Build with `build_sample.bat "" debug` and capture
`OutputDebugString` to get that output.

## Notes

- We only proxy d3d12.dll. dxgi.dll is left to the system.
- The Agility SDK loads fine through the proxy: both test apps export
  `D3D12SDKVersion` / `D3D12SDKPath` and pick up `.\D3D12\D3D12Core.dll`.
  That matters, because the tier we will eventually spoof is reported by
  D3D12Core, not by the System32 stub.
- `sampletest\` is build output and is gitignored.
