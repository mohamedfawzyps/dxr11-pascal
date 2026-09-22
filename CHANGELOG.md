# Changelog

## Versioning

[Semantic Versioning](https://semver.org/). The major version is the honest
part:

- **0.x** while the tier flip is opt-in. The refusal list has only ever been
  exercised against this project's own shaders, so the boundary between "runs"
  and "refused" is measured but not yet trusted.
- **1.0.0** when real software has run through it and that boundary holds.
  That is a statement about exposure, not about features.

MINOR adds lowering coverage or a feature. PATCH is fixes only. The version is
compiled into the DLL and logged on attach, so a log file identifies its own
build.

---

## 0.36.7

- **`createHandleFromHeap` (218) joins the recomputable list**, which closes the
  single largest refusal a real game produces. The 0.25.0 note said a heap
  handle used inside the Proceed loop is refused because 218 is not on that
  list, and filed it as correct. It was measured on a shader that kept all 16
  of its heap handles in the raygen, so nothing ever counted the in-loop case.
  Counted from one Escher run: 58 of 157 refusals are exactly this.
- It is recomputable for the reason the other handles are, one step further
  out. The descriptor heap is set on the command list and is the same heap in
  the any-hit as in the raygen, and the index reaches it from a cbuffer, which
  holds the same bytes for the whole dispatch. The fixpoint enforces the "from
  a cbuffer" half by itself: 218 is admitted only when its index is ALREADY
  recomputable, so an index built from a UAV read or a phi still refuses. It
  needs no conversion, unlike 57 and 217, because a heap handle means the same
  thing in a library, so it is emitted verbatim.
- **Measured on the real corpus, not on the suite.** Replaying the 58 dumped
  Unreal shaders: 18 now lower and go container in to signed container out; 16
  turn out to do a UAV append inside the loop as well and hit the side-effect
  refusal from 0.36.6 instead; 24 still refuse because their chain runs
  through a `rawBufferLoad` or a `textureLoad`. All 18 are byte-identical
  between the Python and the C++, and the two agree on every one of the 58
  outcomes.
- All thirteen render cases are byte-identical to before, 23 dispatch cases
  and four gates pass, 17 of 17 refusal checks.
- **GAP, stated rather than hidden: no RENDER test covers a heap handle inside
  the Proceed loop.** The suite has no case that indexes
  `ResourceDescriptorHeap` there, so what is proven is that 18 real engine
  shaders lower, validate, sign and agree across two implementations, not that
  the result draws the right pixels. A case for that is the next thing to
  build.

## 0.36.6

- **A Proceed loop body that WRITES is now refused.** The loop isolation check
  guards what the body READS, and nothing guarded what it writes. The two are
  not the same question: the any-hit shader the body becomes runs a different
  number of times than the loop does, by design. With
  `RAY_FLAG_FORCE_OPAQUE` no candidate is yielded so it never runs at all, and
  with `FORCE_NON_OPAQUE` it runs once per candidate in an
  implementation-defined order. A store, an append or an atomic there means
  something different after lowering, silently.
- Found on `RayTracingDebugMainCS`, a real UE 5.8.2 shader that appends a
  debug record per candidate. It lowered, validated, signed, and then killed
  the device inside `CreateStateObject`. **Whether the write is also what the
  driver choked on is NOT established and this refusal does not rest on it.**
- The rule comes from the module rather than a hand-kept list of store
  opcodes, which would be incomplete the day DXIL grows another one: DXC marks
  every `dx.op` declaration with an attribute group, and a bare `nounwind`
  writes memory where `readnone` and `readonly` do not. The group numbering is
  resolved per module, not assumed, which is the bug 0.26.0 had to fix once
  already. The `rayQuery_*` ops are exempt because they are rewritten rather
  than transplanted, and the analysis already refuses one it does not know.
- Both implementations, byte-identical, and all thirteen render cases still
  produce identical output, so nothing existing moved.
- `test_reject.py` provokes it by adding a counter update to a known-good
  loop, on the handle that loop already reads through, so the isolation check
  exempts it and the new refusal is what fires. Pointing it at a UAV handle
  instead makes `refuse_uav_in_loop` fire first, which is how the two were
  told apart.
- LIMIT: the check reads `dx.op` calls. A plain `store` reaches only an alloca
  or groupshared in practice, and groupshared around a query is already
  refused.

## 0.36.5

- Video memory is logged immediately before a RayQuery state object is built,
  from `IDXGIAdapter3::QueryVideoMemoryInfo`, local and system segments. The
  one library that kills the device inside `CreateStateObject` in a shipping
  game builds perfectly on the same card offline: alone, sixty times over, and
  with 261 other state objects already held. Every input is therefore
  exonerated and the difference is what else the device has been asked to do.
  Memory is the first thing about a loaded game an empty probe cannot
  reproduce. A measurement, not an argument: if usage is nowhere near budget
  the idea dies in one run.
- `sotest`'s directory walk capped at 64 libraries, so a 261-library test
  silently became a 64-library one and still reported "device still alive".
  Raised to 1024.

## 0.36.4

- The D3D12 debug layer is relayed into the proxy log as it speaks, through
  `ID3D12InfoQueue1::RegisterMessageCallback`. The layer reports through
  `OutputDebugString`, so reading it meant running DbgView beside the game and
  correlating two clocks by eye. That is useless for a device removal: the call
  that kills the device never returns, so nothing can drain an `ID3D12InfoQueue`
  afterwards. A registered callback is delivered synchronously, while the call
  is still inside the runtime, so a message produced inside a call that never
  returns still gets written. INFO and MESSAGE are dropped. Present only when
  the debug layer is running, which for a shipping game means `dxcpl.exe`.
- `CreateStateObject` is announced in the log BEFORE it is called, so the last
  line of a dead log names the call rather than leaving it to be inferred from
  an absence.
- Neither changes behaviour. 23 dispatch cases and all four gates pass.

## 0.36.3

- `rqonly = N` builds ONLY the N-th RayQuery shader that lowers, counting from
  0, and refuses every other one even though it lowered. `rqlimit` can only ask
  "how many", and the bisect it produced ended on a question that is not a
  number: the seventh shader kills the device inside the game and builds
  perfectly offline, with its own root signature, alongside the other six. So
  the next thing to separate is whether that shader alone does it or whether it
  needs the other six present. `rqonly` takes precedence over `rqlimit`.
  A bisect knob, not a setting.

## 0.36.2 (2026-09-22)

### rqlimit counted the wrong thing, and burned two runs doing it

`rqlimit = N` was meant to cap how many state objects exist, so the crash
could be bisected by count. It counted every shader that REACHED `TryCreate`
instead. Those are not the same set, because the first shader Unreal creates
is not the first one that lowers. With `rqlimit = 1` the entire budget went to
a shader the rewriter then refused for loop isolation, so **zero** state
objects were built and the run measured nothing. That happened twice, and only
became visible once 0.36.1 made the phase path log at all.

The gate now sits after the rewrite and before anything is built, so a refused
shader does not spend the budget and `rqlimit = N` means what it says.

**The shape of this mistake is the one the brief keeps recording**: a knob that
looks like it measures the thing, measures something adjacent, and the test
passes without failing. A run that built nothing is indistinguishable from a
run that built something harmless, unless the log says which.

### What the readable log showed

163 RayQuery shaders in Escher's frontend alone. Of the ones actually
attempted, exactly one refusal, and it was loop isolation reading `%v119`. The
real refusal rate is still unknown, because `rqlimit` forwarded the other 162
before the rewriter saw them.

---

## 0.36.1 (2026-09-22)

### A phase made every outcome silent, and it cost a run

0.36.0 gave a REFUSED shader a do-nothing pipeline so the phases would be
comparable. It did that by returning before the log line and before
`shdump::Refused`, so under any `rqphase` the log said nothing per shader at
all. A run with `rqlimit = 1` then could not answer the only question it was
asked: did that one shader lower and build a state object, or was it refused.
The crash/no-crash bit is worthless without it.

Every outcome under a phase is now counted and logged, and a refusal is dumped
as it would be normally:

    rqphase: RayQuery shader N lowered, state object BUILT
    rqphase: RayQuery shader N REFUSED, do-nothing pipeline substituted: <why>

**The rule this breaks is one the brief already states.** When you change what
the product does, re-ask what each test is still measuring. Making the phases
comparable quietly removed the instrument that made them readable.

### The pipeline library wrapper was guarded the wrong way round

`CreatePipelineLibrary` wrapped only when `m_tier11`, which is true when the
REAL hardware already reports Tier 1.1. On such hardware this shim never
produces a stand-in pipeline, so there is nothing to decline; on Pascal, where
stand-ins exist, the library was never wrapped. The whole of
`d3d12_pipeline_library.cpp` was dead code on the only cards it was written
for. Every other guard in that file reads `!m_tier11`.

Found by reading the call site while chasing something else, not by a test,
because no test covers a PSO cache.

---

## 0.36.0 (2026-09-22)

### rqphase = 1 did not exonerate anything, and the experiment was at fault

That run ended with

    LowLevelFatalError [PipelineStateCache.cpp:730]
    Shader compilation failures are Fatal.

rather than a GPU crash, which looks like the DXIL rewrite being cleared. It is
not. A forwarded RayQuery shader is fatal in Unreal, so the run ended early, at
a point that has nothing to do with the crash being bisected. **`rqlimit = 0`
had the identical flaw and it was written down at the time.** Making the same
mistake twice in one investigation is worth recording more than the fix is.

While a phase is set, a REFUSED shader now gets a do-nothing pipeline as well.
Every phase then runs the game equally far, and the only variable is how much
of the lowering happened.

### The second executable is a launcher, not a second renderer

`C:\DW\Escher\Escher.exe` is 176 KB of `BootstrapPackagedGame`: it
`CreateProcess`es `Escher\Binaries\Win64\UE5_Frontend_UI-Win64-Shipping.exe`
and imports neither d3d12 nor dxgi. The shim is beside the right binary.
Checked rather than assumed, because "the other exe" would have invalidated
every run so far.

### The game's own D3D12 runtime makes no difference either

Every offline test used the OS runtime, 10.0.26100; the game loads
`D3D12Core.dll 1.618.5.0` from its own `Binaries\Win64\D3D12\x64`. `sotest` now
exports `D3D12SDKVersion` and `D3D12SDKPath`, loads that exact DLL and prints
which one it got.

All seven libraries still build and are held with the device alive. So the
state object is not the cause on any runtime, at any count, on any number of
threads.

---

## 0.35.0 (2026-09-22)

### DRED says the GPU was executing nothing

`-gpucrashdebugging` reached the game, `RHI.DRED` is `true`, and the report has
**no breadcrumbs and no page fault data**. Aftermath was on and wrote no dump.

That is a result, not a blank. DRED breadcrumbs record command list progress;
none means the GPU was not running tracked work when the device went. No page
fault means nothing touched bad memory. `DXGI_ERROR_DRIVER_INTERNAL_ERROR` with
neither is a **CPU-side driver failure**.

Found by reading the engine source, which had been sitting on this machine
unread for a day: `UE::RHI::ShouldEnableGPUCrashFeature` makes one command line
switch force every GPU crash feature on. Every earlier report said
`RHI.DRED false`, which is why eight runs said the GPU died and none said what
it was doing.

### Also from the source, and tested because of it

`FD3D12PipelineState::CreateAsync` starts an
`FAsyncTask<FD3D12PipelineStateWorker>`, so Unreal creates compute PSOs on
worker threads and this shim's `CreateStateObject` calls are concurrent. Every
test here was single-threaded.

`sotest --threads 8 --repeat 3`: **168 state objects created concurrently on
the 1070, none failed, device alive.** Together with 210 created in sequence,
that rules out one bad library, cumulative creation, resource limits and
concurrency. None of it is worth testing again.

### rqphase, bisecting the middle instead of guessing at it

`rqstub = 1` runs and full lowering crashes, so the cause is between them.
Everything in between has been replayed offline and none of it reproduces. So
stop part-way through instead:

    rqphase = 0   nothing (the same as rqstub)
    rqphase = 1   rewrite the DXIL, throw it away
    rqphase = 2   rewrite, then create the state object
    rqphase = 3   rewrite, state object, then build the shader table
    absent        all of it

A stopped phase keeps everything it built alive and attached to the carrier, so
driver objects and memory match a real run. It simply never registers the
carrier, so no dispatch is substituted and the pipeline does nothing.

Two runs locate it. Measured both ways: `rqphase = 2` gives DIVERGE on every
case in the dispatch suite, which is what "built but never run" looks like, and
unset all 23 pass.

---

## 0.34.0 (2026-09-22)

### Stop handing the application an object D3D12 never made

Unreal never created a pipeline library, so 0.33.0's `StorePipeline` guard was
not the crash either. It is kept, because it is correct, but it was the fourth
consecutive fix of the same shape and the shape is the problem.

`Dxr11RayQueryPso` was an `ID3D12PipelineState` this shim implemented itself
and gave to the application. Every call that can carry one back to the runtime
then has to be found and trapped, and they were found one crash at a time:

    0.x     SetPipelineState
    0.31.0  Reset, ClearState, CreateCommandList
    0.33.0  StorePipeline

Each fix was right and none was the last one, **because there is no list of
every place a pipeline state can go.** An engine can name it, cache it,
serialise it, hand it to an interface this shim does not wrap, or do something
nobody here has thought of.

So the application now gets a **real** pipeline state: its own root signature,
and the do-nothing compute shader 0.32.0 already ships for `rqstub`. The
lowered query is attached to it with `SetPrivateDataInterface`, the same
mechanism the AddToStateObject emulation has used since Phase 4.

The carrier's shader is never executed. `Dispatch` is still intercepted and
still replaced by `SetPipelineState1` plus `DispatchRays`, so what the carrier
contains has never mattered. What matters is that **D3D12 made it**, so every
call the application makes on it is D3D12 handling its own object.

Two things fall out of that, and both are simplifications:

- `Reset`, `ClearState` and `CreateCommandList` forward the pointer again.
  They only look it up now, to know which query the list is bound to.
- Recognising one is a pointer lookup in a small map rather than a private
  `QueryInterface`. The carrier owns the query object, so the entry is removed
  in the query's own destructor and can never outlive it.

### Still not claimed

That this is the crash. But it is a different kind of change from the four
before it: those closed one door each, this removes the corridor.

---

## 0.33.0 (2026-09-22)

### rqstub RUNS the game, so the cause is ours

With every RayQuery shader replaced by a do-nothing compute pipeline, and
everything else unchanged, **the game runs.** Tier 1.1 still claimed, ray
tracing still enabled, Unreal's own DXR 1.0 pipelines and acceleration
structure builds still happening. No GPU crash.

So it is not Unreal driving ray tracing on Pascal. It is something this shim
produces or hands over.

The lighting going white and the hues shifting under `rqstub = 1` is the stub
working: the RayQuery passes write nothing, so the lighting reads empty
buffers. Not a rendering bug, and not evidence of one.

### StorePipeline was the last unguarded door

`Dxr11RayQueryPso` is an `ID3D12PipelineState` that is not a real D3D12 object.
0.31.0 closed `Reset`, `ClearState` and `CreateCommandList`. One more call can
carry one to the runtime, and it does not look like a pipeline call at all:

    ID3D12PipelineLibrary::StorePipeline(name, pPipelineState)

An engine with a PSO cache asks the runtime to SERIALIZE a pipeline state so it
can be reloaded next launch. Unreal has exactly that machinery, in
`PipelineStateCache.cpp`, **which is the file its fatal error named from the
very first engine run.** Handing it an object D3D12 did not create means
reading a vtable and fields that are not there.

`CreatePipelineLibrary` was forwarded untouched, so the application held a real
library and could store anything in it. It is now wrapped, for this one method.
Everything else forwards; nothing is cached, rewritten or inspected here.

A declined store returns S_OK. The application believes it cached the pipeline,
the next launch misses on that name, and it creates it again, which is the
ordinary cache-miss path and one this shim already serves.

### Not claimed

That this IS the crash. It is the last place a stand-in can reach the runtime,
and the coincidence with the named file is suggestive, but the run decides.
What IS established, and was not before: the cause is on this side of the line.

---

## 0.32.0 (2026-09-22)

### nowrap = 1 runs the game, and that narrows it without settling it

Six versions and four real defects into chasing the GPU crash, the control this
brief documents was finally run. **`nowrap = 1` runs normally: no GPU crash.**

So nothing the driver does on its own account is killing it. But that is less
than it looks like, and saying so matters more than the result. `nowrap = 1`
hands over the real device, so Unreal sees Tier 1.0 and switches ray tracing
off entirely. "No crash" is therefore consistent with two different stories:

- this shim's translation is at fault, or
- ray tracing simply did not happen.

`rqlimit = 0` cannot separate them either: a forwarded RayQuery shader is a
fatal error in Unreal, so that run ends before the interesting part.

### rqstub, which separates them

    rqstub = 1

Every RayQuery shader becomes a REAL compute pipeline state that does nothing.
The application is satisfied and carries on, Tier 1.1 is still claimed, ray
tracing stays enabled, **Unreal's own DXR 1.0 pipelines and acceleration
structure builds still run**, and this shim lowers not one shader.

- **Still crashes** the cause is Unreal driving ray tracing on Pascal, not
  anything translated here, and this project cannot fix it.
- **Runs** the cause is in what the lowering produces or in how it is
  dispatched, with everything else held constant.

It is applied on BOTH pipeline creation paths. The stream form is the one that
matters, because that is the only one Unreal uses; a diagnostic that covered
only the struct form would not have touched a single shader in the game.

`proxy/rq_stub_cs.h` is a signed container for
`[numthreads(1,1,1)] void main() {}`, generated once with dxc and checked in,
because the proxy has an assembler and a signer but no HLSL compiler and adding
one for four instructions is not worth it. A shader with no resources is
compatible with any root signature, so it stands in for whatever was being
created.

Measured: with `rqstub = 1` the dispatch suite reports DIVERGE on every case,
which is what "a pipeline that does nothing" should look like, and with it off
all 23 pass.

---

## 0.31.0 (2026-09-22)

### The stand-in PSO was reaching the driver through three other doors

`Dxr11RayQueryPso` is an `ID3D12PipelineState` the application holds and never
inspects. It is not a real D3D12 object, and the driver must never see it.
`SetPipelineState` has always trapped it. **Three other calls take a pipeline
state and none of them did:**

    Dxr11CommandList::Reset(allocator, pInitialState)
    Dxr11CommandList::ClearState(pPipelineState)
    Dxr11Device::CreateCommandList(..., pInitialState, ...)

All three forwarded the pointer straight through, so the driver was handed an
object it did not make and asked to treat it as a pipeline state.

**Resetting a command list with a PSO is the ordinary way to reuse one**, and
it is what Unreal does. This project's harness has always reset with `nullptr`,
which is why every test passed while a real engine hit it on the first frame.

All three now recognise the stand-in, pass `nullptr` to the real call, and
remember it, so a list created or reset with one still dispatches correctly.

### The harness now resets the way an engine does

`raytest` closes and re-resets its list WITH the pipeline state before
dispatching. Put the bug back and the first case fails with
`cmdlist Close (hr=0x80070057)`: the runtime rejecting a list reset with an
object that is not a pipeline state. Without that change the suite passes
either way, which is exactly what it did through five versions of chasing this.

### What this does NOT claim

It is not established that this was the GPU crash. What is established:

- **The generated libraries are fine.** All seven dumped from a crashing run
  build on the 1070 individually, and all seven build and are held at once in
  one process, with the device still alive afterwards. `sotest --dir` does it
  in about a second.
- **They are fine on WARP too**, so they are valid DXR 1.0 rather than merely
  tolerated.
- So the driver dying is not "this library cannot be compiled", which is where
  0.30.0 left it.

Handing the driver a bogus pipeline state is a real defect with a real
mechanism for `DXGI_ERROR_DRIVER_INTERNAL_ERROR`, found by looking at what else
the shim gives the driver rather than at the shaders. Whether it is THE cause
is a question for the next run.

### A measurement that was not clean

The first `sotest` results were taken with a stale 0.16.0 proxy sitting in
`phase5out`, so the probe ran against a wrapped device claiming Tier 1.1
instead of the real driver. The numbers did not change when it was moved aside,
but they were not evidence until they were taken again without it.

---

## 0.30.0 (2026-09-22)

### rqdispatch answered nothing, and the reason IS the answer

`rqdispatch = 0` crashed the GPU exactly as before. But **the log never printed
the `rqdispatch = 0` line**, and that setting is read inside the dispatch path.
Its absence means `DispatchAsRays` was never called.

So no lowered dispatch has ever executed, in that run or any earlier one. The
bisect was a no-op on a code path that never runs, which is why it changed
nothing. Reading the log for what is MISSING was worth more than the
experiment.

That settles the split anyway: **the device dies while state objects are being
created, before a single generated ray is traced.** Six libraries compile, then
the device is gone.

### Capturing the root signature, because a library alone cannot be replayed

`sotest lowered_006.out.dxil` on WARP returns `E_INVALIDARG`: the library
declares bindings and `CreateStateObject` needs a global root signature that
provides them. D3D12 offers no way to get a blob back out of an
`ID3D12RootSignature`, so there was nothing on disk to rebuild one from.

Every root signature an application creates goes through the wrapped device, so
the blob is kept on the way past, keyed by the object that came back. Same
trick `res_tracker` uses for buffers and the same lifetime rule: no reference
is held. The dump now writes `lowered_NNN.rs.bin` beside the pair.

### sotest: one question, one second, no game

`phase5/sotest.cpp`, built by `build_sotest.bat`. It takes the dumped set and
builds the SAME state object `rq_pipeline.cpp` builds, on hardware or on WARP.

The dump writes four files now, because a library and a root signature are not
enough: the hit group TYPES depend on what the lowering produced, and the first
version of sotest inferred those by searching the container for export names.
`lowered_NNN.shape.txt` records what the shim actually decided, and sotest
refuses to run without it rather than guessing and reporting a state object
nobody built.

    sotest lowered_006.out.dxil lowered_006.rs.bin
    sotest lowered_006.out.dxil lowered_006.rs.bin warp

Three runs of a real game have been spent on this and each answered one
question. A driver that falls over falls over here instead, on one shader, with
nothing else running, and `GetDeviceRemovedReason` is asked directly rather
than read out of a crash report afterwards.

WARP first is the useful order: if WARP accepts a library the fault is likely
NVIDIA's, and if WARP refuses it the message says what is actually wrong.

---

## 0.29.0 (2026-09-22)

### The GPU crash is ours, and rqlimit was the wrong instrument

`rqlimit = 0` changed the failure from `Fatal error!` with a GPU crash dump to

    LowLevelFatalError [PipelineStateCache.cpp:730]
    Shader compilation failures are Fatal.

which is Unreal reacting to the driver rejecting a forwarded RayQuery shader.
**No GPU crash.** So the device removal is caused by this shim's substitution,
not by something the game does on its own.

But that is nearly all it establishes, and rqlimit cannot establish more.
Unreal dies at the FIRST forwarded shader, long before the point where the
driver had been dying, so every setting between 0 and "all" ends the run early
for a reason that has nothing to do with the crash being chased.

**This project had already written that down**: "In Unreal, a refusal is a
crash." A bisect built on refusing was never going to work, and the note
explaining why was in CLAUDE.md before the bisect was written.

### rqdispatch, which is the bisect rqlimit should have been

    rqdispatch = 0    build every lowered pipeline, dispatch NONE of them
    rqdispatch = 3    dispatch only the first three, counted in creation order
    absent            dispatch all of them, the normal behaviour

Every state object is still created and every pipeline still handed to the
application, so nothing is refused and the game runs. What changes is only
whether the GPU is asked to execute the lowered work.

That splits the crash in two, and it is the split nothing so far has tested:

- crash survives `rqdispatch = 0`  the driver cannot COMPILE one of the
                                   libraries, and the dump already has all
                                   seven of them
- crash disappears                 the driver cannot EXECUTE one of the
                                   dispatches, and the index narrows it

Rendering is wrong while it is set, which is the point, and it says so.

Measured both ways: with `rqdispatch = 0` the dispatch suite reports DIVERGE
rather than failing, which is what "built but never run" should look like, and
unset all 23 cases pass.

---

## 0.28.0 (2026-09-22)

### rqlimit, a bisect for the driver crash

0.26.0 and 0.27.0 both ended the same way: the shim lowers shaders, the driver
compiles their state objects, and then the device dies with
`DXGI_ERROR_DRIVER_INTERNAL_ERROR`. 0.27.0 got further, six shaders lowered
instead of four, and crashed in the same place.

Nothing in the log says WHICH of them the driver could not survive, and
reasoning about it has produced two plausible stories and no evidence. So:

    rqlimit = 0    substitute nothing, forward every RayQuery shader
    rqlimit = 3    substitute the first three, forward the rest
    (unset)        substitute all of them, the normal behaviour

Shaders are counted in creation order, which is stable enough across runs of
the same scene to bisect with. A forwarded one is logged with its index and the
reason, so the log says exactly what was skipped and why.

This is a diagnostic, not a setting, and it says so when it is on.

Measured both ways rather than assumed: with `rqlimit = 0` the dispatch suite's
first case fails with `CreateComputePipelineState (hr=0x80070057)`, which is
the driver rejecting a RayQuery shader on Tier 1.0, exactly what forwarding is
supposed to produce. Unset, all 23 cases pass again.

### What the dump established

`lowered_006.in.dxil` is 13132 bytes, which is `RayTracingDebugMainCS`, the
same shader dumped as `refused_010.dxil` two versions ago. Dumps are written
before `CreateStateObject`, so 000 through 005 are the six that succeeded and
006 is the one that reported device-removed.

That does NOT make 006 the culprit. Device-removed on a creation call usually
means the device was already gone, so 006 is as likely to be the first call
after the crash as the cause of it.

All seven use `createHandleFromHeap`, so bindless is not what separates them.

---

## 0.27.0 (2026-09-22)

### Dump the shaders that LOWER, not only the ones that do not

A real Unreal session lowered four shaders, built their state objects, and then
the driver died with `DXGI_ERROR_DRIVER_INTERNAL_ERROR`. Those four were the
prime suspects and **none of them existed anywhere on disk**, because `dump`
only ever wrote refusals. There was nothing to replay through
`CreateStateObject` short of launching the game again.

`lowered_NNN.in.dxil` is the application's container, `lowered_NNN.out.dxil` is
the library this shim generated and handed to the driver. The input is what
`dxrw rewrite` needs to reproduce the lowering; the output is what the driver
actually saw, which is the one that matters when the driver is the thing that
crashed.

Written **before** `CreateStateObject`, deliberately. A library that lowers
cleanly and then kills the driver is exactly the case worth having, and dumping
after the call would miss it.

Same `dump` setting and same cap, but its own counter, so a run that lowers a
lot and refuses a little does not lose its refusals to the limit.

Checked rather than assumed: `dxrw rewrite` on a dumped `.in.dxil` reproduces
the dumped `.out.dxil` byte for byte, so what lands on disk is genuinely
replayable.

### The 0.26.0 fixes had not been installed

The run that reported them still failing was `shim 0.25.0`, which the log said
on its first line. The version was bumped and `dxrw.exe` rebuilt, but the proxy
DLL itself was not, so nothing new ran. The `dxgi.dll` beside the game was
older still, at 0.24.0.

Nothing in the tooling catches this. The log prints the version on every run
and that is the evidence, but it takes somebody reading it.

---

## 0.26.0 (2026-09-22)

### Attribute group numbers are the module's, not a constant

The lowering wrote `#1` for nounwind, `#2` for nounwind readonly and `#3` for
noreturn nounwind, because that is how DXC numbered every shader this project
had written. A real Unreal module numbers them differently:

    ours     #0 readnone  #1 nounwind           #2 nounwind readonly  #3 added
    Unreal   #0 readnone  #1 nounwind readonly  #2 nounwind           #3 ABSENT

`#3` was appended by replacing the literal line
`attributes #2 = { nounwind readonly }`, which that module does not contain, so
it was never added. `AnyHitNull` was then marked `#3`, which resolved to
nothing, so `IgnoreHit` was not noreturn and the `unreachable` after it was
illegal:

    error: Instructions must be of an allowed type.
    note: at 'unreachable' in block '#0' of function 'AnyHitNull'.

The message names the instruction and not the attribute that made it illegal,
which is a long way from the cause. Three of six refusals in one real session.

The generated text now writes placeholders that are resolved against the input
module, reusing a group that exists and appending one that does not. Ours still
resolve to 1, 2 and 3, so every existing output is byte-identical.

### An unused declare is a validation error, including ours

`dx.op.dispatchRaysIndex` was declared unconditionally. It only replaces
`threadId`, so a shader that never read `SV_DispatchThreadID` never calls it.
**Every shader in this suite reads it**, which is exactly why this was
unconditional and why nothing here could find it.

### The test that would have caught it did not exist

Every input in the suite agrees with the hardcoded numbering, so no case could
expose it. `test_reject.py` now rewrites a known-good module to the other
numbering before lowering it, and checks that each generated function carries
the id its module actually uses. Put the bug back and that check fails, which
is the only evidence that it is a check at all.

### Measured, on the six shaders a real Unreal session refused

    refused_000  assembler, use of undefined value          still refused
    refused_001  2 RayQuery objects (they are sequential)    still refused
    refused_002  attribute numbering                         NOW LOWERS
    refused_003  loop body reads a caller local              still refused
    refused_004  attribute numbering + unused declare        NOW LOWERS
    refused_005  attribute numbering                         NOW LOWERS

### Not fixed, and not yet explained: the GPU crash

That session ended in a **GPU crash**, not a shader compile failure.
`CrashType GPUCrash`, and `D3DDeviceRemovedReason` is `0x887A0020`,
`DXGI_ERROR_DRIVER_INTERNAL_ERROR`. Four shaders had lowered and built state
objects successfully before it.

Two candidates and no evidence separating them: the refused shaders, which are
forwarded unchanged so a RayQuery DXIL reaches a Tier 1.0 driver, or the
libraries this shim generated. Fewer refusals means less of the first, so this
release reduces exposure without establishing a cause.

The next build is the diagnostic, not a guess: dumping the shaders that LOWER
as well as the ones that do not, so the four can be replayed through
`CreateStateObject` offline on the 1070 without the game.

---

## 0.25.0 (2026-09-22)

### Shader Model 6.6 resource binding

Unreal compiles its RayQuery shaders at cs_6_6, and 6.6 does not reach a
resource the way 6.5 does:

    cs_6_5   createHandle (57, class, rangeId, index)
    cs_6_6   createHandleFromBinding (217, ResBind, index) -> annotateHandle (216)

The rewriter was built against 6.5 and knew nothing about 217, so every Unreal
RayQuery shader failed the loop isolation check: the handles were invisible to
it, which made them look like caller state the any-hit could not see. That one
gap accounted for 124 of the 157 refusals a real Unreal run produced.

The conversion is 217 -> `createHandleForLib` (160), the same target the 6.5
path already uses, and the `annotateHandle` that follows is left exactly as it
stands. What is genuinely different, and could not be guessed from the 6.5
path, is the global: in the binding form it holds a **handle**, not the
resource, so the overload is named after the handle type and the resource
record bitcasts the global back to the resource type. Read off DXC, see
`phase5/cases/reference/lib_sm66_binding_ref.hlsl`.

**A real Unreal shader now lowers, validates and signs.** `RayTracingDebugMainCS`,
dumped from a running game by 0.23.0's shader dump, goes container in and
signed container out through `dxrw rewrite`, and the Python and the C++ produce
byte-identical text for it.

### The module flags were hardcoded, and happened to be right

The lowering wrote `i64 16` as the module's shader flags. Every shader this
project had ever seen declared `0x2000010`, and the one bit the lowering
removes is the tier 1.1 flag `0x2000000`, so 16 was correct by coincidence.

Unreal's shader declares `0x42000010`. The validator said "Flags must match
usage. Flags declared=16, actual=1073741840", which names the symptom and not
the cause.

The flags are now carried through from the entry point's properties with the
tier 1.1 bit cleared. That reproduces `16` exactly for every existing case, so
nothing else moved by a byte.

### Two divergences the port comparison caught

Both were in the C++ and both were invisible until a shader needed record
constants AND ray flags at once, which nothing did before:

- `@rq_record` was declared after the resource globals rather than before;
- the closest-hit read the record before the index fields rather than after,
  and the `rayFlags` declare came after the world-to-object trio rather than
  before.

Neither changes behaviour. Both would have made every later diff noisy, which
is exactly what byte-identity exists to prevent.

### The independent case earned its keep again

`phase5/cases/rayquery_sm66.hlsl` is the `indep` shader at cs_6_6, so it
converts a handle in the raygen AND recreates one in the generated any-hit. It
failed on its first run, in a way the Unreal shader never could: LLVM prints an
all-zero `ResBind` as `zeroinitializer` instead of writing the fields out, and
nothing Unreal bound sat at SRV t0 space0. Fixed in both implementations.

### Still refused, and now named honestly

A resource array reached through a 6.6 binding and indexed dynamically. The
6.5 path supports exactly that, through a getelementptr on the array global.
The 6.6 form is refused only because the shape of an ARRAY global in the
binding form has not been measured off DXC, and guessing it is how a lowering
goes silently wrong.

### Tests

23 render cases and 4 gates in `run_dispatch_test.ps1`, 13 render cases and 15
refusal checks in `run_rewriter_test.ps1`. All bit-exact against WARP, all
byte-identical between the Python and the C++.

---
## 0.24.0 (2026-09-22)

### The dump needed a checkbox

0.23.0 added shader dumping, and the next run had it off, because turning it on
meant hand-editing a line into an `.ini`. A diagnostic that needs a manual step
is a diagnostic that does not run.

It is now a checkbox in the setup tool's Debug options, beside the nowrap
switch: **Save shaders the shim could not translate**. It writes
`dump = refused-shaders`, a relative path, so the folder lands beside the game
whatever the `.ini` is later copied to.

### Where the remaining refusals stand

Two of the four assembler causes are gone. What 0.23.0 measured on Unreal:

| count | reason |
|---|---|
| 118 | loop body reads values defined outside it |
| 33 | 2 concurrent RayQuery objects |
| 6 | unused `rq_cbv0` |
| 1 | assembler, undefined value |

The isolation ones now list real values rather than block labels, so they are
genuine caller locals, not the phi-predecessor bug 0.21.0 fixed.

**Two attempts to reproduce the unused-`rq_cbv0` refusal locally both failed.**
A cbuffer read only inside the Proceed loop lowers and signs, because the
generated hit shader rebuilds the handle and keeps it used. A cbuffer declared
and never read is eliminated by DXC before the rewriter sees it. Recorded
because a failed reproduction is a result: whatever produces it is not either of
the obvious shapes, and guessing further without the actual IR would be
guessing.

### Verified

22 render cases and 4 gates on the dispatch path, 12 render cases and 15
analysis and lowering checks on the rewriter path.

---

## 0.23.0 (2026-09-22)

### Stop guessing at the last refusal

`dump` in the `.ini`, or `DXR_TIER11_DUMP`, writes every refused shader to a
folder as a `.dxil` container with a `.txt` beside it saying why. Off by
default, capped at 64 files.

The point is the round trip: `dxrw rewrite` takes one of those containers and
**reproduces the same refusal offline**, where it can be disassembled and read.
Verified end to end on a known refusal, message for message.

This exists because the last refusal of any size cannot be settled from a log
line. "2 concurrent RayQuery objects in LumenScreenProbeGatherHardwareRayTracingCS"
says what the check decided, not whether it was right, and the check counts
`allocateRayQuery` CALLS rather than whether two queries are ever live at once.
Two queries used one after the other are refused identically. Deciding that
needs the actual DXIL, and the only place it exists is inside the game's
process.

It is the same reasoning as every measurement in this project: prefer the
artefact to the hypothesis. Thirty-three shaders refused for a reason nobody
has read is thirty-three guesses.

### Verified

22 render cases and 4 gates on the dispatch path, 12 render cases and 15
analysis and lowering checks on the rewriter path, the C++ byte-identical to the
Python on every one.

---

## 0.22.0 (2026-09-22)

### The entry block has no label, and a phi can still name it

The last of the assembler failures, and the oldest known defect in the project:

    use of undefined value '%bb0'

An `if` with no `else`, straight out of the entry block, makes the merge
block's phi name the entry as an incoming predecessor:

    %11 = phi float [ 5.000000e-01, %9 ], [ 0.000000e+00, %0 ]

DXC gives the entry block no label, because nothing can branch to it, so
normalisation had nothing to rename. `%0` became `%bb0`, nothing defined it,
and the assembler said so.

The normaliser now adds a label for the entry block **when, and only when, the
function turns out to reference it**. That is legal: an entry block may be
named, it just cannot be branched to. Emitting it only on demand keeps every
module that never needed one byte-for-byte unchanged.

Fixing that exposed a second, smaller thing: both module parsers always created
an implicit entry block, so a labelled first block became a *second* block and
the entry came out empty. A labelled first block is now taken as the entry.

### Why it survived the whole of Phase 5

**Every case in this project opened with `if (tid.x >= width) return;`.** That
guard puts a block between the entry and everything else, so the entry is never
a phi predecessor. One shared habit across every test, hiding one bug, for
about as long as the project has existed.

It is the same shape as the assumption the independent shader found earlier:
a suite made only of cases written to demonstrate a lowering shares whatever
its author took for granted.

`rayquery_entryphi.hlsl` has no such guard and an `if` with no `else`. 8128
hits, bit-exact against WARP.

### Verified

22 render cases and 4 gates on the dispatch path, 12 render cases and 15
analysis and lowering checks on the rewriter path, the C++ byte-identical to the
Python on every one, and the Phase 4 probe unchanged.

---

## 0.21.0 (2026-09-22)

### Two bugs that only a real engine's control flow exposes

0.20.0 closed the runtime ray flags, and the next Unreal run brought loop
isolation straight back, 118 of them, reporting this:

    Proceed loop body reads values defined outside it (%bb1, %bb2, %v33, %v35)

`%bb1` is a **block label**, not a value. A phi writes its predecessors as
`[ %val, %bb12 ]`, with no `label` keyword to mark them, so the operand scan
counted the predecessor BLOCKS as value reads. Unreal's loop bodies are full of
phis, so the check refused them for reading their own blocks.

The bug was invisible for a version, because those same shaders were being
refused earlier for computing their ray flags at runtime. **Closing one refusal
is what exposed the next**, which has now happened four versions running.

The second bug was underneath it. With the phi handling fixed, the shader
lowered and the assembler said:

    Metadata id is already used
    !17 = !{i32 1, !16, !16}

A `[branch]` or `[loop]` hint makes DXC emit
`!16 = distinct !{!16, "dx.controlflow.hints", i32 1}`, and the metadata scan
matched `!N = !{...}` but **not** `= distinct !{...}`. So that id was invisible,
`max(md)+1` started too low, and the fresh-id counter handed out an id the
module already used. Unreal uses those hints constantly.

Both fixed in both implementations, byte-identical.

### The test

`rayquery_phi.hlsl` has real control flow in the loop body, with `[branch]` to
stop DXC flattening it into a `select` — which would produce no phi and test
nothing. That one shader carries both bugs: the phi and the `distinct` node
beside it. 10618 hits, bit-exact against WARP.

### Verified

21 render cases and 4 gates on the dispatch path, 12 render cases and 15
analysis and lowering checks on the rewriter path, the C++ byte-identical to the
Python on every one, and the Phase 4 probe unchanged.

---

## 0.20.0 (2026-09-22)

### Ray flags computed at runtime

0.19.0 closed loop isolation and RayFlags, and the next Unreal run refused 118
shaders on a refusal **this project had added one version earlier**:

    TraceRayInline is given ray flags computed at runtime

That refusal was the right answer to a real bug. `_imm` returns None for a
non-immediate and `(dyn_flags or 0)` had been folding it to 0, so a shader
computing its flags at runtime traced with the WRONG flags and nothing said so.
Refusing beat rendering wrong.

But it was the wrong answer to the question. **`dx.op.traceRay` takes RayFlags
as an ordinary `i32` operand, not an immediate.** Measured, not assumed:
`phase5/cases/reference/lib_dynflags_ref.hlsl` passes a cbuffer value to
`TraceRay` and DXC emits `i32 %7` and signs the container. There was never
anything to fold.

The operand is now passed through, OR'd with the template's flags because
`RayQuery<FLAGS>` means both apply and `TraceRay` takes only one operand:

    %rq.flags = or i32 %v34, 2
    call void @dx.op.traceRay...(i32 157, %dx.types.Handle %v2, i32 %rq.flags, ...)

**What deliberately did NOT change is the classification.** `RayFlags()` on the
query still reports only the statically known bits, which is the honest answer
for deciding whether traversal is provably fixed-function. A query with no
Proceed loop and no `FORCE_OPAQUE` template is still refused, because a runtime
value cannot prove it.

`rayquery_dynflags.hlsl` is the static case with the flags made runtime.
Identical expected answer, 0x202, reached differently: dropping the operand
loses the runtime half and reports 0x002. Bit-exact against WARP, 14450 hits.

### Verified

20 render cases and 4 gates on the dispatch path, 12 render cases and 15
analysis and lowering checks on the rewriter path, the C++ byte-identical to the
Python on every one.

---

## 0.19.0 (2026-09-22)

### Loop isolation, and the primitive underneath it that was wrong

Unreal refused 59 shaders with "Proceed loop body reads values defined outside
it". That is the ordinary shape of a real alpha test: read the thresholds once,
compare against them per candidate.

The refusal was right in principle and wrong here, the same way it was once
wrong about resource handles. The any-hit shader cannot see the raygen's
LOCALS, but some values are not caller state at all, they are facts it can work
out for itself:

    a cbuffer read      bound by the GLOBAL root signature, same bytes for
                        every invocation of the dispatch
    the ray index       DispatchRaysIndex() in the any-hit is the SAME ray
    arithmetic on those pure, so recomputing gives the same answer

So the generated hit shader rebuilds the chain at its top rather than refusing.
`_recomputable` is a fixpoint over a short whitelist, and **what is not on it is
the point**: no loads, because a UAV the raygen wrote would read back
differently; no phis, because a phi depends on which path the raygen took; no
integer division, because recomputing hoists it and division by zero is
undefined; no other `dx.op`, the same rule the opcode whitelist follows.

### The primitive

`Instr.uses()` decodes **call arguments and phi incomings only**. A value
reaching the loop body through ordinary arithmetic, `fcmp float %bary,
%thresh`, was invisible to it. So the isolation check never saw those values,
the generated hit shader referenced ones it never defined, and the ASSEMBLER
reported it as "use of undefined value" — loud, but nowhere near the cause.

That is almost certainly what the 6 assembler failures in Unreal's log were.

There is now a complete operand scan. Types are told from values by asking the
module rather than guessing: a name is a type exactly when the module declares
`%name = type`. Guessing on the shape of the name does not work, because
`%rq.pl` and `%dx.types.Handle` look alike.

### Three things the work caught

- **The scan found values the old one missed, and the check then refused them
  correctly.** The new test slipped past the old check and failed at the
  assembler, which is how the primitive's gap surfaced at all.
- **The chain walked back into the loop.** A value used in the body and also
  defined there was rebuilt in the prologue as well, a duplicate definition.
  The closure now skips what the body defines for itself.
- **Two regexes were silently mangled to nonsense.** `` became a literal
  backspace and the typedef scan matched nothing, so every type looked like a
  value and the whole exemption came back empty. It reported as a plain
  refusal, with no hint that the detector was broken.

### The boundary is tested, not just the feature

`param` in the dispatch suite proves the exemption works, with a cbuffer
threshold and a ray-index value crossing into the loop, 4064 hits bit-exact
against WARP. `refuse_uav_in_loop` in the refusal suite proves it **stops**: a
UAV read used inside the loop is still refused, message for message in both
implementations. An exemption is exactly the kind of thing that widens quietly.

### Verified

19 render cases and 4 gates on the dispatch path, 12 render cases and 15
analysis and lowering checks on the rewriter path, the C++ byte-identical to the
Python on every one, and the Phase 4 probe unchanged.

---

## 0.18.0 (2026-09-22)

### RayFlags, and a silent wrongness it exposed

With GeometryIndex closed, Unreal's next run refused 59 shaders on opcode
**195, `RayFlags`** alone. It had been 1 refusal before; those shaders now get
past GeometryIndex and hit this instead. Largest single remaining bucket, and
the cheapest to close.

**Two places, two different routes**, which is why the test reads it twice:

    inside the loop    dx.op.rayFlags (144), legal in an any-hit and an
                       intersection shader
    outside the loop   the raygen, where that intrinsic is NOT legal. The value
                       is exactly what the lowering hands TraceRay, so it folds
                       to a constant

A lowering that used the constant in both places would pass a test that read it
in only one.

`phase5/cases/rayquery_flags.hlsl` makes the flags a **union** of the template
argument and the `TraceRayInline` argument, `0x002 | 0x200 = 0x202`, so dropping
either half is visible, and the loop read gates the commit so a wrong value
there empties the image. Measured on the 1070: `RayFlags = 514 (0x202)` on
14450 hits, bit-exact against WARP.

**The first version of that test could not have failed.** It used
`CULL_BACK_FACING_TRIANGLES`, which culled the only triangle in the scene, and
both sides reported 0 hits.

### And the bug underneath it

Reading the flags turned up that the lowering had been **silently trusting them
to be a compile-time constant**. `_imm` returns `None` for anything else and
`(dyn_flags or 0)` turned that into 0, so a shader computing its ray flags at
runtime would have traced with the WRONG flags and nothing would have said so.

That is precisely the failure this project refuses to ship, and Unreal is
exactly where it would bite. It is now a refusal with the offending operand in
the message, in both implementations.

### Verified

18 render cases and 4 gates on the dispatch path, 12 render cases plus 14
analysis and lowering checks on the rewriter path with the C++ byte-identical to
the Python on every one.

---

## 0.17.0 (2026-09-22)

### GeometryIndex, the accessor this project called impossible

A real Unreal 5.8 run refused 158 shaders, and 120 of them were four accessors:

| opcode | accessor | refusals |
|---|---|---|
| 203 | `CandidateGeometryIndex` | 105 |
| 214 | `CandidateInstanceContributionToHitGroupIndex` | 12 |
| 209 | `CommittedGeometryIndex` | 3 |
| 215 | `CommittedInstanceContributionToHitGroupIndex` | 0 here |

All four now work, bit-exact against WARP on the GTX 1070.

**The reason they were impossible was true and is now obsolete.**
`GeometryIndex()` in a DXR 1.0 hit shader is ITSELF a Tier 1.1 feature: such a
library sets shader flag 0x2000000 and `CreateStateObject` on the 1070 returns
`E_INVALIDARG`. HLSL exposes no hit-shader intrinsic for the contribution at
all. Both reasons only hold **while the application owns the shader table**.
The shim builds it, so each hit group record carries the two numbers as local
root signature root constants and the hit shader reads them back.

With `MultiplierForGeometryContributionToShaderIndex` at 1 a hit lands on record
`InstanceContribution + GeometryIndex`, and the shim knows both numbers for
every record it writes.

**The gate, measured before any of it was built: SFI0 = 0x0.**
`phase5/cases/reference/lib_localroot_ref.hlsl` reads both values from
`cbuffer ... : register(b0, space1)` and the container carries no feature flag.
That is the whole difference from the intrinsic. Not optional to check: this
project has already been caught once by an accessor that looked ordinary and
was Tier 1.1.

The IR shape turned out to be machinery the rewriter already had: a global of a
named type, `createHandleForLib` (160), then `cbufferLoadLegacy` (59), which is
the same synthesis it does for `@rq_uav0`.

`NO_LOWERING` in `rayquery.py` is now **empty**. Its comment stays, because the
next opcode with no lowering will want somewhere to say so.

### The test had to be built before the feature

Every scene in this project had one geometry per structure, so every correct
answer for `GeometryIndex` was 0 and a lowering that dropped it entirely would
have passed. `--geom` builds one BLAS with four geometries, a quad per
quadrant. On WARP each value appears 4624 times.

**The candidate values gate the commit rather than escaping the loop.** The
first shape tried kept the candidate index in a local and read it after the
`Proceed` loop, and the rewriter refused it, correctly: that is caller state the
any-hit cannot return. Making it decide something inside the loop is both legal
and far stronger. `geometry + contribution != 3` commits, so the missing
quadrant is geometry 3 without `--contrib` and geometry 1 with it. **The gap
moves**, so neither accessor can be a constant. Measured on hardware: 0,1,2
present in one run and 0,2,3 in the other, and the committed contribution reads
0 and 2 across 13872 hits.

### Three bugs the work caught, all by testing rather than reading

- **The `needsRecordConstants` flag was set on one branch only.** Candidate ops
  and committed ops are collected separately, so a check inside either sees half
  of them. It silently missed every `CandidateGeometryIndex` in the file.
- **The payload field list stopped at 9**, so the raygen read `%rq.pl10` with
  nothing defining it. The assembler named it exactly.
- **A refusal test's premise stopped being true.** `test_reject.py` asserted
  that `CommittedGeometryIndex` must be refused, and the suite failed the moment
  it started lowering. That is the third time here a test encoded a fact about
  the world and the fact moved. The case is kept and flipped to an acceptance,
  so the refusal cannot creep back.

### Also

- **The pipeline stream form is covered end to end.** `--stream` creates the
  same shader through `CreatePipelineState`, root signature first, as an engine
  lays it out.
- **Per-record constants come from the instance data**, with the table sized by
  `max(contribution + geometryCount)` rather than `maxContribution + 1`. Two
  instances putting different `(contribution, geometry)` pairs on one record are
  **refused**, because one record answers once. It does not arise in the usual
  layout, where contributions are a running sum of geometry counts precisely so
  records do not collide.
- The payload grew 88 to 92 bytes. `kPayloadBytes` moves together in
  `lower.py`, `rq_lower.cpp`, `rq_pipeline.cpp` and the harness, or
  `CreateStateObject` fails.

### A note about the transparency check

`run_proxy_test.ps1` reported 14381 of 14400 pixels differing, twice, which
looked like a regression in a path the change could not reach. It was the
capture: both bad runs had grabbed 2 and 5 frames instead of 8, and three
consecutive runs at 8 frames all reported 0. **The frame count is the
reliability signal for that oracle**, and a run that captured fewer than 8
should not be believed either way.

### Verified

17 render cases and 4 gates on the dispatch path, 12 render cases plus 14
analysis and lowering checks on the rewriter path with the C++ still
byte-identical to the Python on every one, the Phase 4 probe at
14450 + 2312 + 48774 = 65536, and `D3D12RaytracingHelloWorld` 0 of 14400 pixels
different.

---

## 0.16.0 (2026-09-22)

### Unreal was refusing Pascal by device id, after accepting the tier

The first Unreal Engine 5.8 run that reached the shim produced a clean log and
nothing else: the tier was reported, Unreal read it, and then in sixteen
minutes across two sessions not one acceleration structure was built, no state
object was created and no RayQuery shader arrived.

The reason is five lines of stock engine code, verified in UE 5.7.4 at
`Engine/Source/Runtime/D3D12RHI/Private/Windows/WindowsD3D12Device.cpp`:

```cpp
static bool IsRayTracingEmulated(uint32 DeviceId)
{ ... 0x1B81, // "NVIDIA GeForce GTX 1070" ... }

if (GRHISupportsRayTracing && IsRayTracingEmulated(AdapterDesc.DeviceId))
{
    DisableRayTracingSupport();
    UE_LOG(..., TEXT("Ray tracing is disabled for NVIDIA cards with the Pascal architecture."));
}
```

**A hardcoded PCI device id denylist, and it runs after the tier check.** So
the shim reports Tier 1.1, Unreal sets `GRHISupportsRayTracing`, and two
function calls later reads the device id and turns everything back off. There
is no cvar, no command line flag and no config that guards it: the
`GAllowEmulatedRayTracing` escape hatch older engine versions had was removed.

Everything else was ruled out first, and each of those is worth keeping:

- **The 1070 passes every hardware gate Unreal has**, measured on the real
  device: feature level 12_1, shader model 6.8, resource binding tier 3, wave
  ops, and 64-bit typed atomics. Atomic64 was the expected blocker and is not
  one.
- **The game ran on the SM6 shader platform.** `Escher_PCD3D_SM6.upipelinecache`
  was written during the run.
- **The one positive signal in the log proved less than it looked.** The
  `CreateCommandSignature(DISPATCH_RAYS)` line is keyed off
  `GRHISupportsRayTracingDispatchIndirect`, which comes from the tier alone. It
  showed the tier was believed, not that ray tracing was on.

### The answer is a second proxy, and it lies about exactly one field

`dxgi.dll`, built by `build_dxgi.bat`. The device id arrives through DXGI, not
D3D12, and Unreal calls `IDXGIAdapter::GetDesc` BEFORE its first
`D3D12CreateDevice`, so there is no moment inside the existing shim early
enough to get in front of it.

**Four patched vtable slots, no wrapper.** Measured first, because this project
has been wrong about vtables before: command queues share an image vtable and
command lists get per-object heap ones. DXGI adapters share, and the vtable
lives in `dxgi.dll`'s image, and `IDXGIAdapter4` is the same object with the
same table, so `GetDesc`, `GetDesc1`, `GetDesc2` and `GetDesc3` are four slots
in one place and cover every adapter from every factory. Nothing is ever
wrapped, so `D3D12CreateDevice` keeps receiving the real object and there is no
unwrapping problem to get wrong.

**What is changed:** `DeviceId`, and only on the cards on Unreal's own list.
Default 0x1F08, an RTX 2060: the lowest-end part with ray tracing hardware, so
nothing assumes more performance than exists, and Turing rather than Ampere
because Unreal carries a separate list of Ampere ids.

**What is not changed, and why each one matters:**

- **The vendor id**, so NVAPI still runs.
- **The description string**, because Windows' own driver lookup keys on it.
  `FWindowsPlatformMisc::GetGPUDriverInfo` takes the description and matches it
  against `EnumDisplayDevices`, so lying there breaks a real lookup and gains
  nothing. The card is still called a GTX 1070 everywhere a person can see it.
- **Every capability answer.** Unreal asks NVAPI for shader execution
  reordering, cluster operations, 64-bit atomics and the driver version, and
  every one of those goes to the real device. Shader execution reordering
  correctly reports unsupported.

That last point is the whole design. Unreal never asks the hardware what it is,
only what it can do, and those two questions have different answers here. This
removes a gate keyed on a name; it claims no capability. Same principle as the
rest of the project, which never implements ray tracing and only translates
API shapes.

**Self-tested on install, and rolled back as a unit if the test fails.** The
four slot indices are written out as an enum rather than as magic numbers, but
what makes them safe is that all four are called on a real denied adapter and
checked, along with the fields that must NOT have moved. Any failure restores
every slot and logs that nothing was changed.

`spoof = 0` or `DXR_TIER11_SPOOF=0` turns it off; `spoofid` picks a different
id. **Sensitivity checked both ways**: with the spoof off the adapter reports
0x1B81 again, and with `spoofid = 0x1E04` it reports that instead.

### And then Unreal turned ray tracing on, and found the real gap

With the spoof in, the first run got much further and crashed:

    LowLevelFatalError [PipelineStateCache.cpp] [Line: 730] Shader compilation failures are Fatal.

The shim log for that run is the result, not the crash. Unreal built **8
bottom-level acceleration structures**, created **208 DXR state objects**, all
`hr=0x00000000`, and one `AddToStateObject` was emulated successfully, 201 + 13
into 213 subobjects. A real shipping Unreal 5.8 title doing hardware ray
tracing on a GTX 1070.

And not one line saying a RayQuery shader had arrived.

**`CreatePipelineState`, the pipeline stream form, was not merely failing to
substitute. It was not even detecting.** The walker read a subobject, and if it
was not a shader it gave up:

```cpp
} else {
    // Not a shader, and its size is not knowable from here.
    return;
}
```

A real engine's compute stream puts `ROOT_SIGNATURE` first. So the walk ended
before it ever reached the `CS`, every single time, and every RayQuery compute
shader in a real game went to the Tier 1.0 driver unmodified. The driver
rejected them, and Unreal treats a failed compute PSO as fatal. Line 730 in 5.8
is the compute site; 5.7.4 has the same two `UE_LOG(Fatal)` calls at 584 and
602, a consistent +128 drift.

`proxy/pso_stream.{h,cpp}` walks the whole stream properly and
`CreatePipelineState` now gets the same substitution `CreateComputePipelineState`
has always had. The size of a subobject IS knowable: it is fixed per type.

**The walker was wrong on its first run, and the test caught it.** A subobject
is `alignas(void*) { TYPE tag; Inner payload; }`, so the payload sits at the
natural alignment of `Inner`, not at a fixed offset:

    Inner = ID3D12RootSignature*   tag(4) pad(4) ptr(8)   payload at 8, 16 bytes
    Inner = UINT                   tag(4) uint(4)         payload at 4,  8 bytes

A fixed offset of 8 desynchronises on the first `NODE_MASK` and reads every
byte after it as something it is not. It reported stopping at subobject type
919831536, which is a fragment of a pointer. The sizes now come from
`alignof(T)` and `sizeof(T)` so the compiler owns the answer rather than a
hand-kept table.

**A new suite case, `stream`**, runs the same shader through
`CreatePipelineState` with the root signature first, exactly as an engine lays
it out. 14450 hits, bit-exact against WARP. Without it this substitution would
have been written, shipped and never executed once, which is how the original
defect survived in the first place: the struct form was the only thing the
suite had ever used.

### A bug this found, which would have hidden the whole feature

`dxgi.dll` was demonstrably loaded, from the right directory, and logged
nothing at all. **`fopen_s` and `_wfopen_s` open EXCLUSIVELY.** `d3d12.dll`
opened the log first, so the second DLL's open failed, its fallback failed for
the same reason, and it went silent. A component that is silent looks exactly
like a component that never ran. Both now use `_wfsopen` with `_SH_DENYNO`, and
the logging moved into `proxy/proxy_log.cpp`, shared by both DLLs, which is
what made one log readable as one run.

### Also

The shim's own `d3d12.dll` and the new `dxgi.dll` both carry version resources
now, the setup tool installs and removes `dxgi.dll` alongside the shim and
lists it in the versions panel, and the release zip carries both.

### Verified

Full regression with BOTH proxies in the folder: **15** render cases and 4
gates on the dispatch path, the Phase 4 probe at 14450 + 2312 + 48774 = 65536,
and `D3D12RaytracingHelloWorld` 0 of 14400 pixels different, which is the case
that exercises DXGI and a real swapchain.

---

## 0.15.0 (2026-09-22)

### A log that can be read by whoever it happened to

Version 0.14.0 was diagnosed from a log somebody sent in, and doing that
exposed how little the log said about its own circumstances. It did not say
when anything happened, where one run ended and the next began, or which copies
of anybody else's DLLs were in play. Three changes, all of them about the
report rather than about the shim.

**Every line is timestamped**, in local time, because the person reading it
knows when their game stuttered in local time and not in UTC.

**Each run is bracketed by a marker** naming the executable and its process id:

    2026-09-22 10:20:00.499 [dxr-tier-11-proxy-log] ======== start: UnrealEditor.exe (pid 4872), shim 0.15.0 ========
    ...
    2026-09-22 10:24:31.002 [dxr-tier-11-proxy-log] ========= end: UnrealEditor.exe (pid 4872) =========

The log is appended to across runs on purpose, so a crash leaves its evidence
rather than being overwritten by the next launch. That only works if the
boundary is visible. **A MISSING end marker is itself the finding**: it means
the process never unloaded us, which is what a crash looks like from in here.

**The versions of the four DLLs that are not ours** are reported once, from the
first device creation:

    DXC:     dxcompiler 1.10.2605.37, dxil 1.10.2605.37
    runtime: D3D12Core.dll 1.619.5.0 (C:\Game\Binaries\Win64\D3D12\D3D12Core.dll)
    runtime: d3d12SDKLayers.dll not loaded, so the debug layer is off, which is normal
    runtime: the real d3d12.dll 6.2.26100.9278 (C:\WINDOWS\system32\d3d12.dll)

Three details that are not incidental:

- **The path is the more useful half.** The same file name arrives from
  System32, from beside the exe, or from an Agility subdirectory, and which one
  it was is exactly the question a differing result raises.
- **Asked by handle, not by name, for the real d3d12.dll.**
  `GetModuleHandleW(L"d3d12.dll")` inside a proxy called `d3d12.dll` answers
  with the proxy.
- **Reported after `D3D12CreateDevice`, not from `DllMain`.** The runtime loads
  `D3D12Core.dll` during device creation, so asking earlier would report "not
  loaded" for an Agility SDK that is about to be used. It also means DXC is
  probed on every run rather than only on runs that reach a RayQuery shader, so
  the log answers "is the rewriter usable at all" either way.

Nothing loads anything to answer the question: `GetModuleHandleW`, never
`LoadLibraryW`. Asking whether `D3D12Core.dll` is present must not be what
causes it to be present.

### The shim now has a version resource

It was the ONE file in the game folder with no version at all, sitting next to
four Microsoft DLLs that all have one, which is exactly backwards: the others
are stable releases and this is the part likely to be an old copy somebody
forgot they installed. `proxy/d3d12_proxy.rc` carries it, generated from the
same `version.h` the log uses, so the file and the log cannot disagree.

The strings deliberately do not imitate Microsoft's `d3d12.dll`. A file
claiming to be the real runtime is the wrong thing to find in a support thread
or an anti-cheat report, so it says what it is, including a `Comments` field
that says what it is not.

### The log can go somewhere else

`log` in the `.ini`, or `DXR_TIER11_LOG` in the environment, with `%TEMP%`
still the default. A folder is accepted as well as a file name and gets the
usual name put inside it.

Three decisions worth stating, because each is a way this could have been
quietly useless:

- **A relative path resolves against the shim's own directory**, not the
  process working directory. A game started from Steam does not run where the
  person setting the path thinks it does.
- **A path that cannot be opened falls back to `%TEMP%` and says so** on the
  log's first line. A silent fallback puts the log somewhere nobody is looking,
  which is worse than not moving it at all.
- **It is read from `DllMain`**, which is the one place this project's own
  config rule says not to read files from. The start marker is the first line
  written and it comes from there, so the destination has to be known by then.
  It is a read of one small text file, the same class of call as the `fopen`
  the log already made from the same place, and nothing else resolves that
  early. The rule and its exception are both written down in `proxy/config.h`.

`.ini` values stopped being lowercased, since a path is not a keyword, and the
file is now written and read as UTF-8 so a user name with an accent in it
survives the trip. A hand-edited ANSI file still works: the decode falls back.

### The setup tool reports the same five

A **Versions in the game folder** panel, read off the files before anything is
launched, which the log cannot do. It looks for the Agility SDK in the `D3D12`
subfolder as well as beside the exe, because `D3D12SDKPath` is how applications
normally ship it.

The two halves answer different questions and both are worth having: the panel
says what is installed, and the log says what the process actually loaded.

**The first version of that panel got the Agility SDK wrong**, and it was wrong
in the way that looks like an answer. It guessed a list of subfolder names,
`D3D12` and `D3D12Core`, and reported "none, the game uses the Windows D3D12"
for a game that ships one in `Binaries\Win64\D3D12\x64`. `D3D12SDKPath` is a
relative path the application chooses, so there is no list to guess. It now
searches three levels down and reports where it found the file, which is the
half that turned out to matter.

The log side was never affected: it asks the loader what is loaded, which is
independent of where it came from.

The panel also carries the log location, with `Browse...` and a `Default`
button, and `Open the log` follows whatever is set there.

### Verified

Full regression unchanged: 14 render cases and 4 gates on the dispatch path, 12
render cases plus 9 refusals on the rewriter path with the C++ still
byte-identical to the Python, the probe at 14450 + 2312 + 48774 = 65536, and
`D3D12RaytracingHelloWorld` 0 of 14400 pixels different at 2137 against a 2152
baseline.

The log redirection was checked on all four of its paths: a folder, an explicit
file name, a relative name, and an unopenable one, which fell back to `%TEMP%`
and complained on the first line. The Agility search was checked against the
real game folder that exposed the bug, which now reports 1.618.5.0 found in
`.\D3D12\x64`.

---

## 0.14.0 (2026-09-22)

### The wrapper covered ID3D12Device7. Unreal asks for Device12.

The first test against a real engine, Unreal Engine 5.8, and the shim was
bypassed almost entirely. Not a RayQuery problem: nothing in the lowering ran,
because nothing ever reached it.

| Implemented | Unreal asked for | What happened |
|---|---|---|
| `ID3D12Device7` | Device8 to Device12 | handed over **unwrapped** |
| `ID3D12GraphicsCommandList6` | CommandList7 to CommandList10 | handed over **unwrapped** |

Once an application holds an unwrapped `ID3D12Device8`, every call on that
pointer goes straight to the driver: `CreateComputePipelineState`,
`CreateStateObject`, `CreateCommandList`, and `CheckFeatureSupport`. The tier
was reported once, to the wrapper, and then the application upgraded its
interface and never asked us anything again. Toggling ray tracing in the editor
did nothing because Unreal was reading the real Tier 1.0 off the real device.

Not one `USES RAYQUERY` line appeared in a whole session, despite detection
being hooked on all three pipeline creation paths.

Both wrappers now go to the SDK ceiling, `ID3D12Device15` and
`ID3D12GraphicsCommandList10`, keeping the existing rule that a level the real
device lacks is refused with `E_NOINTERFACE` rather than answered with a vtable
it cannot honour. `Barrier` and `DispatchGraph` close a queued indirect
dispatch first, as the other work-recording methods do.

**This was a known hole that was measured as harmless and was not.** The
project had already caught `D3D12RaytracingSimpleLighting` asking for
`ID3D12GraphicsCommandList5` and noted that nothing else in the test apps did
that. Everything else in the test apps was a Microsoft sample from 2018.

So the startup log now names the ceiling on every run:

    highest device interface available: ID3D12Device15 (this shim implements up to 15)

A GTX 1070 on a current driver offers Device15, so the gap was eight interface
versions wide.

### Verified

Full regression unchanged: 14 render cases, 4 gates, the probe at
14450 + 2312 + 48774 = 65536, and `D3D12RaytracingHelloWorld` 0 of 14400 pixels
different. The raytest run now logs no passed-through interfaces at all, where
before it was silent only because the harness never asked for one.

---

## 0.13.0 (2026-09-22)

### Renamed everything that read as DirectX 11

`dxr11` parses as "DirectX 11" on sight, which is the opposite of what this
does. The repository was renamed to `pascal-dxr-tier-1.1` for that reason and
the rest had not caught up.

| Was | Is |
|---|---|
| `dxr11.ini` | `dxr-tier-11.ini` |
| `dxr11.example.ini` | `dxr-tier-11.example.ini` |
| `DXR11_TIER11` | `DXR_TIER11` |
| `DXR11_NO_WRAP` | `DXR_TIER11_NOWRAP` |
| `DXR11_DEBUGLAYER` | `DXR_TIER11_DEBUGLAYER` (test harness only) |
| `%TEMP%\dxr11_proxy.log` | `%TEMP%\dxr-tier-11-proxy.log` |

**A clean break, with no compatibility shims for the old names.** None of these
were ever in a published release, so nothing exists to be compatible with.
Accepting both would be carrying a name this project renamed on purpose.

Internal C++ identifiers are deliberately untouched. `Dxr11Device`,
`Dxr11CommandList` and the rest are not user-visible, and renaming them would
be a very large diff for no reader's benefit.

Earlier entries in this file were also left alone. Those releases really did
use the old names, and rewriting the history would make it describe things that
never happened.

### Verified

Three behaviours re-checked by name after the sweep, since a rename that
silently stops reading a setting looks exactly like a working default:
`dxr-tier-11-proxy.log` is written, `DXR_TIER11=0` turns the tier off, and
`tier11 = 0` in `dxr-tier-11.ini` does too.

Full regression unchanged: 14 render cases, 4 gates, 12 rewriter cases
byte-identical on both paths, 14 analysis checks, the probe at
14450 + 2312 + 48774 = 65536, and `D3D12RaytracingHelloWorld` 0 of 14400 pixels
different with the wrapper running.

---

## 0.12.0 (2026-09-22)

### Without DXC the shim now stands aside completely

0.11.0 stopped claiming Tier 1.1 when `dxcompiler.dll` and `dxil.dll` are
missing. This goes further: it stops wrapping the device at all.

Everything the wrapper still handles is a Tier 1.1 feature, `AddToStateObject`,
indirect `DispatchRays`, and the command list and acceleration structure
machinery those need. Reporting Tier 1.0 means an application that reads the
tier it was given never calls any of them, so the wrapper would sit in the path
of every device and command list call waiting for work that cannot arrive.

That is risk with no benefit, and the risk is not theoretical: the wrapper
hooks a queue vtable and wraps every command list.

    standing aside entirely: dxcompiler.dll is not next to the shim ... The
    application gets the real device untouched, which is what it would have had
    with no shim installed at all.

The log survives, because the lines that say what happened come from the proxy
layer rather than from the wrapper.

**The cost:** DXC is now loaded at device creation rather than at the first
RayQuery shader. The lazy loading noted in earlier work is gone when DXC is
present. Two `LoadLibrary` calls at startup, in exchange for doing nothing at
all when it is absent.

### A test had quietly stopped testing anything

`run_proxy_test.ps1` compares a Microsoft sample with and without the shim and
asserts the frames are pixel identical. Its whole purpose is to show the
WRAPPER is transparent.

The sample folders contain no DXC, so after the change above the shim stood
aside there and the comparison became the unmodified path against itself. It
would have passed forever while proving nothing.

The script now copies DXC beside the proxy, and warns loudly if it cannot. Both
samples are verified again with the wrapper actually running.

That is the fourth time in this project a test was found measuring nothing, and
the first where a correct change to the product caused it.

### Renamed

The log prefix is `[dxr-tier-11-proxy-log]`, since `dxr11` reads as DirectX 11.

---

## 0.11.0 (2026-09-22)

### Tier 1.1 is no longer claimed without DXC

Claiming Tier 1.1 is a promise to translate RayQuery, and that promise cannot
be kept without `dxcompiler.dll` and `dxil.dll`. The shim now checks, and
reports the real tier when they are absent.

This was the exact failure the project refuses to ship, and 0.10.0 had
introduced it by making the tier claim the default: copy the DLL on its own,
forget the other two files, and an application is told it may emit RayQuery,
does so, and the driver rejects a shader nobody could rewrite. The application
crashes and the cause is only in a log, after the fact.

Verified both directions in a directory containing nothing but the executable
and `d3d12.dll`:

    [dxr-tier-11-proxy-log] NOT reporting Tier 1.1: dxcompiler.dll is not next to the
    shim ... Tier 1.0 is reported instead, which is honest

then with the two DXC files added, the claim returns. NOT in the automated
suite: it needs a directory without DXC and the test harness compiles its own
shaders with DXC. Verified by hand, and said plainly here rather than implied.

The cost is that DXC now loads at the first feature query rather than at the
first shader. That is the point: better to find out early.

### Renamed

`tools/dxr11-setup.*` is now `tools/dxr-tier-11-setup.*`. "dxr11" reads as
DirectX 11 at a glance, which is the opposite of what this does.

---

## 0.10.0 (2026-09-22)

Makes the thing usable without a terminal. No change to any translation, and
the whole regression is unchanged.

### Tier 1.1 is now ON by default

The opt-in already happened. A proxy DLL only sits beside an executable
because somebody deliberately put it there, and that is the consent. Requiring
a second one, through an environment variable that a launcher never passes on,
mostly produced reports that the shim does nothing.

Turning it off is still one step: delete the DLL, or `tier11 = 0`.

The gate this replaces was worth having during development, where the risk was
a test run silently flipping. It is the wrong default for a release.

### Settings live in a file now

`dxr11.ini`, beside the DLL. A game started from Steam or the Epic launcher
never sees an environment variable you set in a console, so a file is the only
mechanism that works regardless of how something is launched. See
`dxr11.example.ini`.

Environment variables of the same name still work and **take priority**, so a
stray `.ini` can never change what the test scripts measure.

### The log says which DXC produced a shader

    [dxr-tier-11-proxy-log] rewriter using dxcompiler 1.10.2605.37, dxil 1.10.2605.37

The first question about any signing or validation failure is which compiler
was involved, and the log could not answer it. Read from the file version
resource rather than `IDxcVersionInfo`, which reports only major and minor,
because a bug report needs the build number to name a release.

### Verified

The default flip is the part that could have broken something, so the case
that matters is a DXR 1.0 application now being told Tier 1.1 with no
configuration at all. `D3D12RaytracingHelloWorld` still renders 0 of 14400
pixels different at unchanged fps, with the log confirming Tier 1.1 was
reported.

Everything else unchanged: 14 render cases, 12 rewriter cases byte-identical
on both paths, 14 analysis and lowering checks, the probe at
14450 + 2312 + 48774 = 65536.

The gate tests were rewritten rather than deleted. They used to assert the
flip could not happen by accident. They now assert the off switch works, by
environment variable and by file, which is what a bug report will be asked to
try first.

---

## 0.9.0 (2026-09-21)

First tagged release. A `RayQuery` compute shader, unmodified, runs on a GTX
1070 and produces bit-exact output against WARP.

This is a snapshot taken deliberately **before** pointing the shim at a real
engine, so that whatever the engine turns up can be measured against a fixed
point.

### Tier 1.1 features

- **Ray flags** (`SKIP_TRIANGLES`, `SKIP_PROCEDURAL_PRIMITIVES`) need no shim.
  Measured against WARP: the Tier 1.0 driver already honours them.
- **`AddToStateObject`** emulated by stripping `ALLOW_STATE_OBJECT_ADDITIONS`,
  caching the subobjects and rebuilding a whole new state object.
- **Indirect `DispatchRays`** emulated by splitting the command list at the
  dispatch, reading back the argument buffer and issuing real `DispatchRays`
  calls. Binding state survives the split.
- **Inline ray tracing** by rewriting the DXIL into a DXR 1.0 library with
  generated any-hit, intersection, closest-hit and miss shaders.

### Inline ray tracing coverage

- Opaque closest hit, alpha-tested closest hit, shadow/visibility, procedural
  primitives, and a query committing both triangle and procedural hits from one
  `Proceed` loop
- `Abort()`, carried as a payload flag
- 21 of the 25 `RayQuery` accessors Unreal Engine 5.7 uses. The other four have
  no DXR 1.0 equivalent
- Resource arrays at constant, dynamic and non-uniform indices
- Acceleration structure interception, so the shader table is sized and typed
  from the application's own geometry layout rather than assumed

### Verification

All on a GTX 1070, with WARP as the oracle on every run rather than a stored
baseline:

- 14 end-to-end render cases, all bit-exact, plus 2 refusal gates
- 12 rewriter cases, each byte-identical between the Python reference and the
  C++ port, on both the `.ll` and DXIL container paths
- 9 refusals provoked by tests
- Two Microsoft DXR 1.0 samples unchanged through the proxy, one of them
  pixel-identical to its no-proxy baseline

### Known issues

Recorded because they are known, not because they are acceptable:

- **A `RayQuery` shader with no early-out bounds check fails to lower.** The
  entry block becomes a phi predecessor and normalization leaves a dangling
  label reference. It fails at the assembler with a message that names nothing
  relevant. Every shader in the test suite happens to open with a bounds check,
  which is why this was not caught earlier.
- **`CreatePipelineState`** (the pipeline stream form) detects `RayQuery` but
  forwards it instead of lowering it. Only `CreateComputePipelineState` gets
  the substitution.
- **`createHandleFromHeap`** is documented as refused and is not. A bindless
  SM 6.6 shader is accepted and fails later for the reason above.
- **More than one `RayQuery` object per entry point is refused**, including
  plainly sequential ones. Primary plus shadow in a single compute shader is a
  common real shape.
- **No real application has run through this yet.** Unreal Engine coverage is a
  source survey, not an execution test, and the survey found Epic using all
  four of the accessors that cannot be lowered.

### Notes

The tier flip stays opt-in behind `DXR11_TIER11=1`. Reporting Tier 1.1
entitles an application to emit `RayQuery`, and a shim that claims 1.1 and then
fails is worse than one that claims 1.0.
