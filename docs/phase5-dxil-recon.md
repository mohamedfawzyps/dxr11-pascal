# Phase 5 recon: what the RayQuery DXIL actually looks like

Phase 5 is the DXIL rewriter, the months-long half of the project. Following
the working method in the brief, this is reconnaissance before design: read the
real bytes first, and prefer a grep over a hypothesis.

Everything below is observed from DXC 1.10.2605.37 (`c4d8f4f99`) on the dev
machine, not taken from a specification.

## Method

Phase 2 already wrote both sides of the transform by hand and proved them
equivalent bit-exactly on real hardware. So the rewriter's input and its
required output both already exist, validated, in `phase2/raytest.cpp`. That
makes them the specification.

`tools/extract_shaders.py` pulls the four HLSL literals out of `raytest.cpp`
into `phase5/shaders/`, rather than copying them, so the shader Phase 5 works on
cannot drift from the one Phase 2 validated. They are compiled with the same
profiles `raytest.cpp` uses, `cs_6_5` for the RayQuery side and `lib_6_3` for
the TraceRay side, and disassembled to `phase5/dxil/*.ll`.

    python tools/extract_shaders.py

## Verified: the opcode numbers

`AllocateRayQuery` is **178**, as the brief said, now confirmed against this
DXC rather than assumed. The full set appearing in our two inputs:

| Opcode | Name | Seen as |
|---|---|---|
| 178 | AllocateRayQuery | `dx.op.allocateRayQuery` |
| 179 | RayQuery_TraceRayInline | `dx.op.rayQuery_TraceRayInline` |
| 180 | RayQuery_Proceed | `dx.op.rayQuery_Proceed.i1` |
| 182 | RayQuery_CommitNonOpaqueTriangleHit | `dx.op.rayQuery_CommitNonOpaqueTriangleHit` |
| 184 | RayQuery_CommittedStatus | `dx.op.rayQuery_StateScalar.i32` |
| 185 | RayQuery_CandidateType | `dx.op.rayQuery_StateScalar.i32` |
| 193 | RayQuery_CandidateTriangleBarycentrics | `dx.op.rayQuery_StateVector.f32` |
| 194 | RayQuery_CommittedTriangleBarycentrics | `dx.op.rayQuery_StateVector.f32` |
| 200 | RayQuery_CommittedRayT | `dx.op.rayQuery_StateScalar.f32` |

### The first design consequence: dispatch on the immediate, not the name

Look at the collisions. 184 and 185 are **the same LLVM function**,
`dx.op.rayQuery_StateScalar.i32`, and so are 193 and 194 as
`dx.op.rayQuery_StateVector.f32`. CommittedStatus and CandidateType differ only
in the opcode immediate in operand 0.

So matching on the callee name is wrong and would silently confuse a committed
accessor with a candidate one, which is the difference between reading the final
hit and reading the hit currently being considered. The rewriter must read
operand 0. Recording this because it is exactly the sort of thing that would
otherwise be discovered as a wrong render.

## Verified: the query object is an SSA integer, not memory

    %33 = call i32 @dx.op.allocateRayQuery(i32 178, i32 0)
    call void @dx.op.rayQuery_TraceRayInline(i32 179, i32 %33, ...)
    %34 = call i1 @dx.op.rayQuery_Proceed.i1(i32 180, i32 %33)

`RayQuery<>` does not become an `alloca` or a struct. It is a plain `i32`
threaded through every operation as operand 1. That is much better than it could
have been, and it settles three things at once:

- **Which query an op belongs to** is answered by following one SSA value. No
  alias analysis, no memory dependence.
- **Detecting multiple concurrent queries**, which the brief lists as having no
  valid lowering, is just counting distinct `allocateRayQuery` results.
- **The template flags are a literal constant** in operand 1 of the allocate,
  here `0` for `RAY_FLAG_NONE` and `1` for `RAY_FLAG_FORCE_OPAQUE`. The dynamic
  flags are a separate operand on TraceRayInline. The brief's mapping, that the
  two OR together into the `TraceRay` RayFlags argument, is confirmed by
  inspection.

## Verified: `while (q.Proceed())` is loop-rotated, and there are two Proceeds

This is the finding that most changes the design. The source has one `Proceed()`
call. The DXIL has two:

    ; %12:
      %34 = rayQuery_Proceed(180, %33)
      br i1 %34, label %35, label %48      <- guard, peeled into the preheader

    ; %35:  br label %36                    <- preheader
    ; %36:  CandidateType(185)              <- LOOP HEADER, preds = %44, %35
             br i1 ..., label %39, label %44
    ; %39:  CandidateTriangleBarycentrics(193), alpha test math
             br i1 ..., label %46, label %44
    ; %46:  CommitNonOpaqueTriangleHit(182)
             br label %44
    ; %44:  %45 = rayQuery_Proceed(180, %33) <- LATCH
             br i1 %45, label %36, label %47
    ; %47:  br label %48
    ; %48:  CommittedStatus(184), ...       <- after the loop

Textbook loop rotation: the guard is peeled into the preheader and the real test
lives in the latch. A rewriter that looks for "a loop whose condition is
`Proceed`" will not find one, because in the IR there is no single such loop
condition. It has to recognise the rotated shape, and treat the two `Proceed`
calls as one source-level construct.

This gives a precise definition of the thing the whole project rests on:

> **The any-hit shader body is the set of blocks reachable from the loop header
> without passing through the latch's `Proceed`.** Here that is `%36`, `%39` and
> `%46`.

And accept versus reject is a reachability property, not a flag:

> **Paths that reach the latch through the `CommitNonOpaqueTriangleHit` block
> accept. Paths that reach it otherwise reject.**

## Verified: the loop body transplants almost literally

Comparing the RayQuery loop body against the hand-written any-hit shader that
Phase 2 proved equivalent, the arithmetic is instruction-for-instruction the
same:

| RayQuery loop body | Hand-written any-hit |
|---|---|
| `%40 = StateVector(193, %33, i8 0)` | `%8 = load attr->barycentrics` then `%9 = extractelement %8, 0` |
| `%41 = fmul fast float %40, 4.0` | `%10 = fmul fast float %9, 4.0` |
| `%42 = Frc(22, %41)` | `%11 = Frc(22, %10)` |
| `%43 = fcmp fast olt float %42, 0.5` | `%12 = fcmp fast olt float %11, 0.5` |
| falls through to the latch | `ret void` |
| reaches the latch via Commit(182) | `IgnoreHit(155)` then `unreachable` |

Only the operand source changes. `CandidateTriangleBarycentrics(q, i)` becomes
an `extractelement` of a load from the `attr` parameter; everything downstream is
untouched. That is a strong result: the body does not need to be understood,
only re-rooted.

Note the polarity. In the RayQuery form, committing is the special path and
falling through rejects. In the any-hit form, returning accepts and `IgnoreHit`
is the special path. The mapping inverts which side is the exception, so this is
an easy place to get the sense backwards. Phase 2's bit-exact result is what
says the intended direction is right.

## Verified: the deltas between a compute module and a library module

The input is one compute entry point. The output is a library with four.
These are the differences the rewriter has to close, whichever way it works.

| | INPUT `cs_6_5` | TARGET `lib_6_3` |
|---|---|---|
| `!dx.shaderModel` | `{"cs", 6, 5}` | `{"lib", 6, 3}` |
| `!dx.entryPoints` | 1 | 5, a null resource-only record plus 4 named |
| `!dx.typeAnnotations` | absent | present, for the payload and attribute structs |
| resource globals | `* undef` placeholders | real globals with mangled names |
| function names | `main` | `\01?RayGen@@YAXXZ`, MSVC mangled |
| signatures | `void ()` | payload and attribute pointer parameters |
| ray index | `dx.op.threadId(93)` | `dx.op.dispatchRaysIndex(145)` |

Entry property tags, read off both files and self-consistent between them:
tag 0 shader flags, tag 4 numthreads, tag 5 auto binding space, tag 6 payload
size in bytes, tag 7 attribute size in bytes, tag 8 shader kind. Shader kinds
observed: 7 raygeneration, 9 anyhit, 10 closesthit, 11 miss. Payload size 16
matches `struct Payload { float t; float2 bary; uint hit; }` and attribute size
8 matches the built-in triangle attributes, so the reading is consistent.
INFERRED from observation, cross-checked across two files, not read from a
specification.

## The measurement: DXIL survives a text round trip

The fork above was posed as scaffold-and-splice against emit-from-scratch. It
was the wrong fork, and measuring first is what showed that. The real question
underneath it is what LEVEL the rewriter works at, because that decides the cost
of every route.

`phase5/dxilrt.cpp` (build with `build_phase5_tool.bat`) answers it. DXC exposes
`IDxcAssembler::AssembleToContainer`, which turns `.ll` text back into a DXIL
container, and Phase 1 already established that `dxil.dll`'s `IDxcValidator`
signs anything that validates. So a container can go out to text and back.

    dxilrt roundtrip <in.dxil>     disassemble, reassemble, sign, compare
    dxilrt asm <in.ll> [out.dxil]  assemble arbitrary IR, sign, report
    dxilrt parts <in.dxil>         list the container parts

All four Phase 2 shaders round trip. Every instruction and metadata line
survives; the diff is entirely comment lines.

| shader | original | round trip | IR ignoring comments |
|---|---|---|---|
| rayquery_opaque (cs_6_5) | 4788 | 4308 | identical |
| rayquery_alpha (cs_6_5) | 5076 | 4592 | identical |
| traceray_opaque (lib_6_3) | 6368 | 5880 | identical |
| traceray_alpha (lib_6_3) | 7236 | 6712 | identical |

### What is lost, and whether it matters

The containers shrink, so something does go. Precisely:

- **`STAT` shrinks.** Reflection commentary. Resource names become empty and
  struct layouts become `[32 x i8] (type annotation not present)`. Recoverable
  only from the original container, not from the text.
- **`VERS` is dropped entirely** from libraries, 40 bytes recording which
  compiler built it.
- **`DXIL` shrinks by ~60 bytes**, consistent with value names.

What is preserved matters more:

- **`PSV0` byte-identical** (156 bytes). This is pipeline state validation, what
  the runtime checks bindings against.
- **`RDAT` byte-identical** (492 and 616 bytes). This is the library runtime
  data listing exports and their properties, and it is exactly what
  `CreateStateObject` reads to find our shaders. If this had been damaged the
  whole approach would be dead.

So the loss is reflection metadata, which tools use and the runtime does not.
An application that calls `ID3D12ShaderReflection` on a shader we rewrote would
see empty resource names. Worth recording as a known limitation; nothing tested
so far does that.

### Proven end to end, on hardware

Signing is not running, so this was checked against real output rather than
trusting the validator. `raytest.exe --roundtrip` sends every shader the Phase 2
harness compiles out to `.ll` and back before D3D12 ever sees it, then runs the
existing WARP against GTX 1070 comparison:

    baseline      opaque 14450 hits, alpha 8117 hits   ALL MATCH
    --roundtrip   opaque 14450 hits, alpha 8117 hits   ALL MATCH

A round-tripped `lib_6_3` builds a working raytracing state object on the 1070
and renders bit-exactly. A round-tripped `cs_6_5` RayQuery shader runs correctly
on WARP. Both sides of the eventual transform survive the trip.

### The sensitivity check, and how it was wrong first

A passing round trip means nothing unless the harness could have noticed a
change. `--rtpoison` makes one edit to the disassembly, flipping the ray
direction from -Z to +Z so every ray points away from the scene.

The first version poisoned every shader, and reported **MATCH**. The edit had
plainly worked, hits went from 14450 to 0, but both sides were corrupted
identically so they still agreed with each other. The verdict was insensitive
even though the shaders were not. Fixed by poisoning only the lowered side:

    --rtpoison    ground truth 14450 hits, lowered 0 hits    DIVERGE
    --rtpoison    ground truth  8117 hits, lowered 0 hits    DIVERGE

So a single line of text changed in the IR propagates through reassembly,
validation, signing, state object creation and rendering, and the harness catches
it. That is also the project's first real DXIL rewrite, small as it is.

## What this settles

**Phase 5 is a text transformation.** Not LLVM 3.7 bitcode surgery. That removes
the single largest cost in the original estimate, and it removes the need for a
bitcode writer entirely. `dxilrt asm` is the feedback loop: generate `.ll`, and
the validator says yes or says exactly what is wrong.

It also dissolves the fork rather than deciding it. Neither route survives
contact with the result:

- Emitting a container from scratch is pointless when the input module already
  has the correct resources, handles, `dx.op` declarations and types.
- A DXC-compiled scaffold plus an IR splice is unnecessary machinery when the
  destination module can simply be the source module.

**The route is to edit the input module in place.** Change `!dx.shaderModel`
from `cs` to `lib`, rename `main` to a mangled raygen export, replace the
`rayQuery` operations with a `traceRay` call, append three small functions, and
rewrite `!dx.entryPoints` with `!dx.typeAnnotations`. The application's own code
never moves between modules, which also means its resource bindings and handles
cannot be got wrong in transit.

INFERRED, not yet tested: that a `cs` module edited into a `lib` module will
validate. The known obstacles are visible in the recon table above, notably
`dx.op.threadId` (93) not being legal in a raygen and needing to become
`dx.op.dispatchRaysIndex` (145), and libraries requiring `!dx.typeAnnotations`.
Targeting `lib_6_5` rather than `lib_6_3` would keep `!dx.version` at 1.5 where
the RayQuery input already sits; the 1070 reports shader model 6.7, so that
costs nothing.

## Pattern 1 lowered by hand, and it runs

`phase5/hand/make_lib.py` turns `rayquery_opaque.ll`, a `cs_6_5` compute shader
using `RayQuery<RAY_FLAG_FORCE_OPAQUE>`, into a `lib_6_5` DXR library. It is a
script rather than a hand-typed file so it is reproducible and so the edits are
individually legible, but every edit was worked out by hand against the
validator. This is pattern 1 from the brief, the stated first target.

    python phase5/hand/make_lib.py
    phase5out\dxilrt.exe asm phase5/hand/rayquery_opaque_lib.ll out.dxil

### Letting the validator specify the work

The first attempt changed one thing, `!dx.shaderModel` from `cs` to `lib`, and
asked. That is the point of having the feedback loop: the validator enumerates
the job rather than being guessed at.

    error: Opcode ThreadId not valid in shader model lib_6_5(lib).
    error: Resource handle should returned by createHandle.
      at '%1 = call ... @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)'
    error: store should be on uav resource.
    error: buffer load/store only works on Raw/Typed/StructuredBuffer.

Both root causes were predicted by the recon. The rest were knock-on effects of
the handle one.

### The edits

- `!dx.shaderModel` `cs 6,5` to `lib 6,5`. Keeping 6.5 rather than dropping to
  6.3 leaves `!dx.version` at 1.5 where the input already sits, and the 1070
  reports shader model 6.7, so it costs nothing.
- **Resources become globals.** A library's handles come from
  `createHandleForLib` (160) applied to a loaded global, not `createHandle` (57)
  applied to a binding index. So three `external constant` globals are added,
  `!dx.resources` points at them instead of `undef`, and the three
  `createHandle` calls become a load plus `createHandleForLib`.
- `dx.op.threadId` (93) becomes `dx.op.dispatchRaysIndex` (145).
- **The query collapses to one `traceRay`.** With `FORCE_OPAQUE`, `Proceed`
  never yields a candidate, so there is no loop and no any-hit shader. The
  `allocateRayQuery` / `TraceRayInline` / `Proceed` / `CommittedStatus` sequence
  becomes a payload init, a `traceRay` (157), and a load of the payload.
- `ClosestHit` and `Miss` are appended. Both are tiny.
- `!dx.typeAnnotations` is added and `!dx.entryPoints` is rewritten from one
  compute record into a resource-only record plus three export records.

Two things did NOT need doing, both worth recording because they were expected
to be work:

- **The CFG is untouched**, phi nodes and all. `CommittedStatus` was being
  compared against `COMMITTED_TRIANGLE_HIT` (1), and the generated `ClosestHit`
  writes `hit = 1` while `Miss` writes `0`, so loading the payload's hit field
  is the identical test. The branch, the three float phis and the integer phi
  all survive as they were.
- **Names do not need MSVC mangling.** DXC emits `\01?RayGen@@YAXXZ`, but plain
  `@RayGen` works and `RDAT` exposes it under exactly that name, which is what
  the state object looks up.

### The constraint that bit

LLVM numbers unnamed values sequentially, so every value this script introduces
is named. The reverse also bites and was not anticipated: **deleting a numbered
value breaks the sequence.** Removing the rayQuery values `%33` and `%34` made
the assembler refuse:

    shader: instruction expected to be numbered '%33'

Fixed by having two replacement instructions take those exact numbers rather
than renumbering the rest of the function. An automated rewriter has to do the
same, or renumber wholesale. Cheap to hit, cheap to fix, and the assembler says
precisely what is wrong.

### Result: it validates, signs, and renders correctly

    assembled ok
    unsigned  5332 bytes, 5 parts: SFI0(8) RDAT(380) STAT(2104) HASH(20) DXIL(2728)
    validated and signed ok

And on hardware, against WARP's native Tier 1.1 RayQuery as ground truth:

    WARP RayQuery (ground truth)          65536 rays, 14450 hits
    hand-lowered library on the GTX 1070  65536 rays, 14450 hits

    hit/miss mismatches: 0
    value  mismatches  : 0 (tol 0.0010)
    max |dt|           : 0.000000
    max |dbary|        : 0.000000
    RESULT: MATCH

**A RayQuery shader, transformed at the DXIL text level, running correctly on
Tier 1.0 hardware.** That is the Phase 5 premise demonstrated end to end, for
one pattern.

`raytest.exe --lib <file.dxil>` is what makes this checkable: the TraceRay side
loads a pre-built library from disk instead of compiling HLSL, so anything the
eventual rewriter produces can be held against the same ground truth.

### Checked for sensitivity, twice

A `--lib` path that was silently ignored would produce exactly the result above,
so the test was made to fail on purpose. Flipping one constant in the
hand-lowered library, the ray direction from -Z to +Z:

    poisoned hand-lowered lib   65536 rays, 0 hits
    hit/miss mismatches: 14450   RESULT: DIVERGE

And as a control in the other direction, DXC's own `traceray_opaque.dxil` pushed
through the same `--lib` path gives 14450 hits and MATCH. So the path is real,
it carries the file it is given, and it notices when that file is wrong.

## Next

Pattern 1 is proven by hand. The two remaining questions, in order:

1. **Pattern 3, the any-hit case.** `rayquery_alpha.ll` has the rotated
   `Proceed` loop, so it exercises the loop-body extraction that pattern 1
   skipped entirely. That is the part of the transform with no precedent here,
   and doing it by hand first will expose whatever the recon missed.
2. **Only then, automate.** The edits above are mechanical but they are keyed to
   exact instruction text. A rewriter has to find the same sites structurally,
   by following the query handle and matching opcodes on operand 0, which is
   what the recon says the IR supports.

Still unknown, and worth settling before automation: whether an application's
shader that binds resources differently, uses descriptor tables rather than root
descriptors, or declares more than one query, still fits this shape. Everything
here is one shader with three root-level bindings.
