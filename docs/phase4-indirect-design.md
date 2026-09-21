# Phase 4: indirect DispatchRays, design decision

**Status: not started. This is a scope and architecture write-up, deliberately
produced before any code.**

Indirect DispatchRays is the last of the three non-shader Tier 1.1 features, and
the one the brief flagged as carrying the real engineering cost. Starting it
turned up two problems that are worth settling deliberately rather than
discovering halfway through an implementation. The second is the larger of the
two and is not the one the brief anticipated.

## Problem 1: the interception point is outside everything the shim wraps

`ExecuteIndirect` is a method on `ID3D12GraphicsCommandList`, not on the device.
The shim currently wraps the device and nothing else, on purpose. Reaching this
call means reaching past that line for the first time.

### Verified

- `ExecuteIndirect(ID3D12CommandSignature*, UINT MaxCommandCount,
  ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset,
  ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset)`, d3d12.h:7027.
- Method counts to wrap: `ID3D12GraphicsCommandList4` is 77 including inherited
  (`IUnknown` 3, `ID3D12Object` 4, `ID3D12DeviceChild` 1, `ID3D12CommandList` 1,
  `GraphicsCommandList` 51, then 6, 1, 1, 9). `ID3D12CommandQueue` is 20.
  About 97 forwarders in total.
- Wrapping the list forces wrapping the queue, because the app hands lists to
  `ID3D12CommandQueue::ExecuteCommandLists` and a real queue cannot accept our
  wrapper.
- **The app hands the command queue to DXGI.**
  `DeviceResources.cpp:321` calls
  `m_dxgiFactory->CreateSwapChainForHwnd(m_commandQueue.Get(), ...)`. We proxy
  `d3d12.dll` only; `dxgi.dll` is left to the system, as recorded in
  docs/phase3-proxy.md.
- `CreateCommandSignature` with `DISPATCH_RAYS` fails on the 1070 with
  `E_INVALIDARG`, and the debug layer names the tier requirement exactly. See
  docs/phase4-probe.md.

### Inferred, NOT verified

- That DXGI rejects or misbehaves with a foreign `ID3D12CommandQueue`
  implementation. This is the crux of the decision and it is currently a
  hypothesis. It is very likely, since DXGI needs the real object to build a
  swapchain, but it has not been measured.
- That all command lists of a given type share one vtable, which is what makes a
  single vtable hook cover every list. Also very likely, and also not measured.

Both are cheap to settle, see "What to measure" below.

### Options

**A. Hook the single `ExecuteIndirect` vtable slot.**
Patch one entry in the real command list's vtable. No wrappers, no unwrapping,
the app keeps real pointers everywhere, DXGI is untouched, and the change is
contained to one function.
Against: it writes into memory the D3D12 runtime owns, through `VirtualProtect`,
and the effect is process-global. It can interact with other things that hook
the same surface, such as Steam, Discord or NVIDIA overlays, and with PIX. It is
also a technique the project has not used anywhere else, so it introduces a
second architectural idiom.

**B. Wrap the command list and the queue.**
Follows the brief's "wrap `ID3D12Device5` and friends". About 97 mechanical
forwarders, the same kind of work as the device wrapper, which went cleanly.
Against: it needs unwrapping at every boundary where a list or queue crosses
back into D3D12, and, unless the DXGI inference above is wrong, it also needs a
`dxgi.dll` proxy so the queue can be unwrapped on the way into
`CreateSwapChainForHwnd`. That is a second proxy DLL and a new export table to
get right, which is real scope beyond this feature.

**C. Wrap, with a QueryInterface escape hatch.**
Wrap the queue but have its `QueryInterface` return the *real* queue for
`IID_ID3D12CommandQueue`, so DXGI gets something usable without a DXGI proxy.
Against: it violates COM identity, and any code path that re-queries the
interface silently bypasses the shim. Silent bypass is the failure mode this
project has repeatedly chosen to avoid. Not recommended.

## Problem 2: the CPU-readback premise may not hold where it matters

This is the more consequential finding, and it is not about interception at all.

The brief's approach for this feature is "CPU readback of the argument buffer,
then direct `DispatchRays`". That works when the argument buffer contents are
known to the CPU. It does not work when the argument buffer is produced by the
GPU, which is the reason indirect dispatch exists in the first place: a compute
shader culls or counts work and writes the dispatch dimensions, so the CPU never
learns them.

For a GPU-written argument buffer:

- Reading at record time is simply wrong. The work that fills the buffer has not
  run yet.
- Reading correctly needs a submit and a CPU wait at the point of the
  `ExecuteIndirect`, then a direct `DispatchRays` recorded afterwards. You
  cannot record into a command list that has already been closed, so the shim
  has to have split the recording into segments from the start, and replay the
  binding state, root signature, root parameters, descriptor heaps and pipeline
  state, at the head of each new segment, because `Reset` clears all of it.
- The split also perturbs resource state tracking across the boundary, which is
  the specific hazard the brief names.

So the feature really has two tiers of difficulty:

| Case | Cost |
|---|---|
| Argument buffer CPU-written, contents stable at record time | Moderate. Read and convert in place, no split. |
| Argument buffer GPU-written | High. Command list segmentation, full binding-state replay, a CPU sync per indirect dispatch. |

The Phase 4 probe only exercises the easy case: one command, no count buffer, an
upload-heap argument buffer written before recording. It proves the API shapes,
not that the hard case is solved.

### ANSWERED: Unreal uses the hard case, and only the hard case

Measured in the local UE 5.7 NvRTX clone at `C:\DW\UnrealEngine`. The path is
`DispatchRays()` in
`Engine/Source/Runtime/D3D12RHI/Private/D3D12RayTracing.cpp:6575`, reached from
`FD3D12CommandContext::RHIRayTraceDispatchIndirect`.

The call shape is the simple one:

    RayTracingCommandList()->ExecuteIndirect(
        CommandSignature, 1, DispatchRaysDescBuffer, offset, nullptr, 0);
                          ^                                        ^
                          MaxCommandCount 1              no count buffer

But the argument buffer is not readable by the CPU at record time, and not by
accident. Lines 6591-6658 build it on the GPU timeline:

- `DispatchRaysDescBuffer` is a per-queue scratch buffer created at
  `D3D12Device.cpp:414` as a DEFAULT-heap buffer, `EBufferUsageFlags::DrawIndirect`
  plus `ALLOW_UNORDERED_ACCESS`, initial state `IndirectArgs`. GPU resident.
- UE copies the **SBT portion**, the first `offsetof(D3D12_DISPATCH_RAYS_DESC, Width)`
  = 88 bytes, from upload memory into it with `CopyBufferRegion`.
- UE then copies the **dispatch dimensions**, the trailing 16 bytes, out of the
  application's own argument buffer, which is itself GPU-produced. The barriers
  around it move that buffer from `SRVGraphicsNonPixel | IndirectArgs` to
  `CopySrc` and back, which is what a GPU-written indirect args buffer looks
  like.
- Both copies are recorded into the same command list, immediately before the
  `ExecuteIndirect`.

So at the moment the shim would see `ExecuteIndirect`, **none** of the argument
buffer is valid yet. Not the dimensions, not even the SBT pointers. The brief's
"CPU readback of the argument buffer, then direct DispatchRays" does not work
for the one real engine we have to hand.

One detail worth keeping, because it bounds the problem: the only genuinely
GPU-originated data is Width, Height and Depth, three uints. Everything else UE
puts in that buffer it already had on the CPU, and a shim that tracked
`CopyBufferRegion` from upload memory could in principle reconstruct it. The
irreducible dependency is 12 bytes that do not exist until the GPU has run.

## What to measure before deciding

Three cheap experiments, none of which require shim code:

1. **Does DXGI accept a foreign `ID3D12CommandQueue`?** Hand
   `CreateSwapChainForHwnd` a minimal stub implementation and see what it
   returns. Settles option B's real cost.
2. **Do command lists share a vtable per type?** Create two lists on one device
   and compare the first pointer-sized word. Settles whether option A is one
   hook or many.
3. ~~What does a real engine put in the argument buffer?~~ **Answered above.**
   GPU-written, so problem 2 is blocking, not theoretical.
4. **How much does Unreal actually lean on this path?** Which render features
   call `RHIRayTraceDispatchIndirect`, and whether they can be turned off. If it
   is one optional feature, the shim could report Tier 1.1 and fail loudly on
   that path while everything else works. If it is on the main Lumen or path
   tracer route, the feature is mandatory.

## Revised constraints (user, this session)

Performance now matters, and dropping a feature is not an option. That rules out
the "fail loudly on this path" escape and makes the strategy choice a
performance question rather than only a complexity one.

With identical results required there are exactly **two** always-correct
strategies, because the dispatch dimensions genuinely do not exist until the GPU
has run:

**S1. Sync per dispatch.** Split the command list at the `ExecuteIndirect`,
submit, wait on the CPU, read the 104-byte desc, then record a direct
`DispatchRays`. Always identical. Costs a full pipeline bubble at every indirect
ray dispatch.

**S2. Over-dispatch with a shader early-out.** Launch a bound G, and have a
rewritten raygen read the real dimensions from the argument buffer and return
immediately when `DispatchRaysIndex()` is past them. No sync, one dispatch,
identical output **provided G is genuinely >= actual**. The raygen rewrite needs
the Phase 5 DXIL rewriter, which is being built anyway, so that machinery is not
wasted.

### The catch with S2: the bound exists, but not where the shim can see it

Unreal's dispatch dimensions come from a ray compaction pass. The compacted
count is bounded by the allocation it compacts into,
`LumenReflectionTracing.cpp:957`:

    NumCompactedTraceTexelDataElements =
        ReflectionTracingBufferSize.X * ReflectionTracingBufferSize.Y * ClosureCount;

So a provably safe bound does exist, and it is resolution-derived and CPU-known
*to Unreal*. The shim, however, sees only D3D12 traffic: a buffer creation, some
`CopyBufferRegion` calls and an `ExecuteIndirect`. Nothing in that stream says
which allocation bounds which dispatch. Deriving it would be engine-specific
guesswork, which is exactly the kind of silent-wrongness this project has
avoided everywhere else.

That leaves three honest ways to obtain G:

- **Configured.** The user supplies a cap, for instance render resolution times
  rays per pixel. Provably safe if correct, and it is a number the person
  deploying the shim can actually know.
- **Learned, with GPU verification.** Use the previous execution's value times a
  margin, and have the shader raise a flag when actual exceeded G. The flag is
  read a frame later, so a violation is *detected and reported* rather than
  silent, but the frame it occurred on was still wrong. Not sufficient on its
  own under an identical-results requirement.
- **Sync on the first execution, cached thereafter.** Correct only if the
  dimensions never grow, which for a resolution-driven compaction they can,
  on a resolution change.

## Recommendation, revised after the Unreal finding

The segmentation work dominates, so the interception mechanism is now the
smaller half of the decision rather than the crux.

What a correct implementation has to do, for the Unreal pattern:

1. Intercept `ExecuteIndirect` on a DISPATCH_RAYS signature.
2. Close the current command list segment there, submit it, and wait on the CPU.
   The queue is not known at record time, so this cannot happen during
   recording; the wrapper has to record into a sequence of segments and do the
   submit and wait at `ExecuteCommandLists` time.
3. Read the 104-byte `D3D12_DISPATCH_RAYS_DESC` back.
4. Record a fresh segment containing `SetPipelineState1`, the replayed compute
   root signature, root parameters and descriptor heaps, and the direct
   `DispatchRays`.
5. Replay enough binding state for everything the app records afterwards,
   because `Reset` clears all of it.
6. Keep resource state consistent across the break.

Steps 2 and 5 are the expensive ones. Step 5 in particular means the command
list wrapper has to track every binding call, not merely forward it, which is a
different and larger job than the device wrapper was.

A per-dispatch CPU sync is acceptable here; the brief says 1 fps is a fine
outcome. The complexity is the problem, not the runtime cost.

On interception, if forced to choose today: option A, the vtable hook, on the
grounds that the DXGI boundary makes option B leaky rather than merely laborious.
But note that option A only gives a clean hook for `ExecuteIndirect`, while the
segmentation in step 2 needs `ExecuteCommandLists` and every binding call too. A
hook-only approach would end up hooking most of the surface a wrapper would have
covered anyway. **If segmentation is going ahead, option B plus a dxgi proxy is
probably the more honest architecture after all.**

### Answered: Unreal leans on this path heavily

`RHIRayTraceDispatchIndirect` callers in `Engine/Source/Runtime/Renderer/Private`:
Lumen reflections, Lumen short-range AO, Lumen stochastic lighting and mega
lights, the path tracer, and RTXDI sampled lights. This is the main hardware ray
tracing route, not an optional extra. The feature is mandatory.

## Decide this with a measurement, not a preference

The sync cost only matters relative to frame time, and frame times on Pascal
running software DXR will be large. A 1 to 2 ms bubble is painful at 120 fps and
close to irrelevant at 10 fps. Likewise, S2's waste is not the ratio G/actual: a
thread that reads three uints and returns costs almost nothing next to a thread
that traces a ray on shader cores, so a 10x over-dispatch is nowhere near 10x
the cost.

Both numbers are measurable on this hardware with the existing probe harness,
and neither is currently known. Proposed next step, before committing to S1 or
S2:

1. Extend `phase4/tier11probe.cpp` with a timing mode that runs the same ray
   workload three ways on the 1070: direct `DispatchRays` as the baseline; a
   split with a CPU sync in the middle, to price S1's bubble; and an
   over-dispatch at 2x, 4x and 10x with an early-out in the raygen, to price
   S2's waste.
2. Report wall-clock per iteration for each.

That produces the actual trade-off curve instead of an argument about it. If
S1's bubble turns out to be a few percent of a Pascal frame, it wins outright on
being unconditionally correct with no configuration. If it costs tens of
percent, S2 with a configured cap earns its complexity.

## Not decided, not started

No shim code has been written for this feature. `CreateCommandSignature` still
forwards unchanged and still fails on Tier 1.0, which is the correct, honest
behaviour until the emulation exists.

## Measured on the GTX 1070 (2026-09-21)

`tier11probe.exe hw -time`, 1048576 rays over a 1024x1024 grid, 40 iterations.
The grid is squared up to the ray count on purpose: an earlier run used a
256x256 grid with 1M rays, so most rays landed outside the scene and missed
instantly, and the "baseline" measured almost nothing.

    direct DispatchRays (baseline)        0.837 ms
    S1  split + CPU sync + dispatch       1.136 ms    +35.8%
    S2  bounded shader, G = 1x            0.969 ms    +15.8%
    S2  bounded shader, G = 2x            1.048 ms    +25.3%
    S2  bounded shader, G = 4x            1.244 ms    +48.7%
    S2  bounded shader, G = 10x           1.787 ms   +113.6%

The absolute numbers belong to a one-triangle scene and mean little. These two
coefficients are properties of the mechanism and do carry over:

    S1 fixed cost per split        0.300 ms
    S2 cost per early-out thread   0.1007 ns

So for a frame with D indirect ray dispatches of A rays each, over-dispatched
to G*A:

    S1 overhead = D * 0.300 ms                  independent of scene cost
    S2 overhead = D * (G-1) * A * 0.1007 ns     plus a tax on every real thread

Break-even is at **G of about 3.8x** for a million rays per dispatch. Below
that, over-dispatch is cheaper; above it, the sync is.

### What this says

**S1 looks like the better strategy for Unreal's pattern**, for three reasons:

1. Its cost is fixed per dispatch and does not scale with the over-dispatch
   factor, so it degrades gracefully.
2. Lumen's dimensions come from a *compaction* pass, and the only provably safe
   bound is the uncompacted allocation. Compaction exists precisely because the
   surviving ray count is far below that allocation, often by 10x or more. That
   is exactly the regime where S2 is worst: at G = 10x it already costs nearly
   four times a split.
3. S2 taxes every real thread as well, not just the wasted ones. The G = 1x row
   is already +15.8% over baseline purely for the bound check.

S1 also needs no configured cap and no raygen rewrite, and is unconditionally
identical rather than identical-if-the-cap-holds.

### The caveat this measurement does NOT cover

The 0.300 ms is a **floor**, measured on an idle GPU with almost nothing in
flight. In a real engine the damage from a mid-frame CPU sync is not the
round-trip time, it is the loss of CPU-ahead pipelining: the recording thread
cannot continue until the GPU catches up, which in a deeply pipelined renderer
can cost far more than the round trip.

So the honest position is: S1 wins on the measured axis and on correctness, but
its worst case is unmeasured and is the main risk. Worth measuring before
committing, with a harness that keeps real work queued behind the split.

If that turns out badly, the mitigation is to reduce D rather than switch
strategy: batch several indirect dispatches behind a single sync where their
ordering allows it, so the frame pays one bubble instead of one per dispatch.

## Measured again, this time pipelined (2026-09-21)

`tier11probe.exe hw -pipeline -filler 4 -frames 60`. Three frames in flight, GPU
work about 2.3 ms per frame, CPU recording cost swept with a synthetic busy-wait.

    CPU ms/frame    baseline    with split    overhead
          0.0       2.514         2.544       +1.2%
          1.0       2.274         3.327      +46.3%
          2.0       2.292         4.240      +85.0%
          4.0       4.167         6.229      +49.5%
          8.0       8.206        10.280      +25.3%

The model holds exactly. Baseline tracks **max(CPU, GPU)**; with a split it
tracks **CPU + GPU**. So the real cost of one sync is **min(CPU, GPU)**, the
overlap that was lost, not the 0.300 ms round trip the idle measurement showed.
The overhead peaks where CPU and GPU are balanced and falls off either side,
which is why the percentage column is not monotonic.

### Splits do NOT saturate

I guessed the damage would saturate after the first split, since the pipeline is
already collapsed, and that batching dispatches behind one sync would therefore
buy nothing. **That guess was wrong.** With CPU fixed at 2 ms:

    splits   with split    vs baseline
         1       4.178        +81.8%
         2       4.933       +114.7%
         4       6.925       +201.4%

Roughly 0.8 to 1.0 ms per additional split. Each one drains the GPU and leaves
it idle while the CPU reads back and records the next chunk, so the gaps add up.
Batching several indirect dispatches behind a single sync is worth building
after all.

## This reverses the earlier recommendation

The previous conclusion, that S1 wins, rested on its cost being a fixed 0.300 ms
per split. Pipelined, it is not: it is min(CPU, GPU) for the first split plus
about 0.9 ms for each one after.

Recomputing break-even with the pipelined numbers, for the measured
configuration (CPU 2 ms, GPU 2.3 ms, 4 dispatches, 1M rays each):

    S1  = +4.64 ms per frame
    S2  = D * (G-1) * A * 0.1007 ns  +  D * A * 0.13 ns  (real-thread tax)
        = 0.40 * (G-1) ms + 0.52 ms

    break-even at G of about 11x, not the 3.8x the idle measurement implied.

And Unreal's case is more favourable to S2 than that, because A is the
*compacted* count. The wasted threads are (allocation - actual), so if a full
resolution allocation of 2M compacts to 200k survivors, each dispatch wastes
1.8M threads, which is 0.18 ms. Five such dispatches cost under 1 ms per frame,
against several ms for S1 on a frame where the CPU has real recording work to do.

**So on performance, S2 wins, and not marginally.** What it still lacks is a
bound the shim can derive on its own, which is a correctness problem, not a
performance one.

## Recommendation

Build both, with S1 as the default:

- **S1 is the correctness floor.** Always identical, no configuration, works
  with no knowledge of the engine. Ship it first and make it the default so the
  shim is never wrong out of the box.
- **S2 is the performance path, opt in with a configured cap.** Whoever deploys
  the shim knows the resolution and the ray budget, which is exactly the number
  needed. Pair it with a GPU-side assertion that raises a flag when the real
  count exceeds the cap, read back a frame later and logged loudly, so a bad cap
  is noisy rather than silently wrong.
- **Batch dispatches behind one sync in S1**, now that the split sweep shows the
  damage is per-split rather than one-off.

That satisfies all three constraints: no feature dropped, identical results by
default, and a measured path to performance for anyone who can state a bound.

### Reproducibility

Re-run three times on a machine that was not idle during the first pass, to make
sure background load was not producing the effect. Total CPU load 7% at the
start of the repeat.

| CPU ms | baseline, 3 runs | with split, 3 runs | overhead |
|---|---|---|---|
| 1.0 | 2.274 / 2.251 / 2.261 | 3.327 / 3.325 / 3.343 | +46.3 / +47.7 / +47.8% |
| 2.0 | 2.292 / 2.285 / 2.294 | 4.240 / 4.219 / 4.183 | +85.0 / +84.6 / +82.4% |
| 4.0 | 4.167 / 4.166 / 4.175 | 6.229 / 6.255 / 6.286 | +49.5 / +50.2 / +50.6% |
| 8.0 | 8.206 / 8.181 / 8.183 | 10.280 / 10.267 / 10.265 | +25.3 / +25.5 / +25.4% |

Stable to a few tenths of a percent for CPU at or above 1 ms, so the
max(CPU, GPU) against CPU + GPU result is solid. The split sweep repeats too:
one split 4.18 / 4.16 / 4.32 ms, four splits 6.93 / 6.76 / 6.22 ms. Noisier at
four, but never near flat, so "the damage does not saturate" holds.

The CPU = 0 row is the only genuinely noisy one, +1.2 / +3.2 / +13.3%, which is
expected: with no CPU work there is no overlap to lose, so it is measuring
something close to zero and the percentage swings on noise.

The idle-GPU numbers repeated as well: S1 per split 0.300 then 0.318 ms, S2 per
early-out thread 0.1007 then 0.0925 ns, break-even G about 3.8x then 4.3x.
Pipelined break-even recomputed from the repeat run lands at G of about 11.5x,
matching the first pass.
