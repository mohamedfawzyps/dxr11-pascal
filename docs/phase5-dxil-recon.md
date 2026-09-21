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

## Pattern 3 lowered by hand: the Proceed loop becomes an any-hit shader

`phase5/hand/make_lib_alpha.py` turns `rayquery_alpha.ll` into a `lib_6_5`
library. This is the one the project rests on. Pattern 1 had no loop and no
any-hit shader at all; pattern 3 has both.

    WARP RayQuery (ground truth)               65536 rays, 8117 hits
    hand-lowered any-hit lib on the GTX 1070   65536 rays, 8117 hits

    hit/miss mismatches: 0
    value  mismatches  : 0 (tol 0.0010)
    max |dt| 0.000000    max |dbary| 0.000000
    RESULT: MATCH

### The numbering problem, and the pass that removes it

Pattern 1 hit the edge of LLVM's positional numbering and worked around it by
having replacement instructions take the numbers the deleted ones had. Pattern 3
cannot do that: lowering the loop **deletes six basic blocks** from the raygen,
which would renumber most of the function.

`phase5/hand/llnorm.py` fixes this once and for all by giving everything an
explicit name before any editing happens:

    %33 = call ...           ->  %v33 = call ...
    ; <label>:44             ->  bb44:
    br i1 %45, label %36     ->  br i1 %v45, label %bb36

After that nothing is positional and blocks can simply be dropped. The
normaliser was checked on its own first: normalising `rayquery_alpha.ll` and
reassembling it still validates and signs, unchanged. **Any automated rewriter
wants this pass**, and it is cheaper than teaching the rewriter to renumber.

### What the lowering actually does

Six blocks disappear from the raygen: the preheader, the loop header, the
candidate block, the latch, the commit block, and the exit. In their place, a
payload init and one `traceRay`. The driver now runs the traversal the
`Proceed` loop was stepping through by hand, and calls the any-hit shader for
each non-opaque candidate.

The any-hit body is the loop body, re-rooted:

| Proceed loop body | generated any-hit |
|---|---|
| `CandidateTriangleBarycentrics(q, 0)` | `extractelement(load attr, 0)` |
| `fmul fast 4.0`, `Frc`, `fcmp olt 0.5` | identical, unchanged |
| `CommitNonOpaqueTriangleHit` | fall off the end |
| fall through to the latch | `IgnoreHit` then `unreachable` |

Two details that are easy to get wrong:

- **The polarity inverts.** Committing is the special path in RayQuery;
  ignoring is the special path in any-hit. Accept is the fall-through in one and
  the explicit case in the other.
- **The `CandidateType()` test is dropped.** The source checks
  `CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE`, which is a tautology
  inside an any-hit shader, since that is the only thing it is ever invoked for.
  A `CANDIDATE_PROCEDURAL_PRIMITIVE` arm would instead have to become an
  intersection shader, which pattern 3 does not exercise and nothing here has
  tested.

`IgnoreHit` is `noreturn nounwind`, and a compute module has no attribute group
for that, so one is appended.

### Checked for sensitivity, including the specific documented trap

The hit count carries a free check: **8117 is not 14450.** If the any-hit shader
had not been wired into the hit group, or had never run, every triangle hit
would have been accepted and the count would be the opaque one.

The polarity inversion above is the mistake the recon warned about, so it was
performed deliberately. Swapping the accept and reject branches:

    polarity inverted    65536 rays, 6333 hits
    hit/miss mismatches: 14450    RESULT: DIVERGE

And **8117 + 6333 = 14450 exactly.** The accepted and rejected sets partition
the opaque hit count with nothing left over, which is independent confirmation
that the alpha test is being evaluated per candidate and that the any-hit shader
governs the outcome. Neither number was arranged; they fell out.

## The rewriter: automated, and it matches the hand lowerings

`phase5/rewriter/` finds the sites structurally instead of matching instruction
text, and reproduces both hand lowerings with no input-specific knowledge.

    python phase5/rewriter/dxrewrite.py analyze <in.ll>
    python phase5/rewriter/dxrewrite.py lower   <in.ll> <out.ll>
    .\tools\run_rewriter_test.ps1        end to end, against WARP

| file | what it is |
|---|---|
| `dxil.py` | a deliberately small .ll model: blocks, instructions, dx.op decoding, and real dominator and natural-loop analysis |
| `rayquery.py` | finds and classifies the query, and refuses what has no lowering |
| `lower.py` | the transform, driven entirely by the analysis |
| `dxrewrite.py` | CLI |
| `test_reject.py` | proves the refusals fire |

### The three structural handles the recon promised

All three held up, which is the main result here.

**Follow the handle.** `allocateRayQuery` returns a plain `i32`, so every
operation belonging to a query is one def-use hop away. No alias analysis, and
counting distinct allocations is all that multi-query detection needs.

**Dispatch on the opcode immediate.** Never the callee name. `dx.op.rayQuery_
StateScalar.i32` is both CommittedStatus (184) and CandidateType (185), so a
name-based match would silently confuse the final hit with the candidate under
consideration.

**Find the loop with dominators.** `Function.natural_loops` finds back edges
where the header dominates the latch. On `rayquery_alpha.ll` it reports header
`bb36`, latch `bb44`, body `bb36, bb39, bb46`, which is exactly what the recon
worked out by hand. There is no textual pattern that finds this, because the
guard is peeled into the preheader and the source's single `while` condition
does not exist as one thing in the IR.

### Results

    pattern 1, opaque closest hit               14450 hits   MATCH
    pattern 3, alpha-tested, generated any-hit   8117 hits   MATCH

Both bit-exact against WARP, 0 mismatches, max |dt| and max |dbary| both
0.000000. Byte for byte the same outcome as the hand lowerings, produced
without any of their hardcoded instruction text.

Pattern 2 also lowers and renders correctly, from an input built by setting
`ACCEPT_FIRST_HIT_AND_END_SEARCH` on the opaque shader: it classifies as
pattern 2, assembles, signs, and matches. **Caveat, stated rather than glossed:
this scene has a single layer of triangles, so accept-first-hit is
indistinguishable from closest-hit in it.** That result shows the path works,
not that first-hit semantics are preserved where they would differ.

### The refusals, and why they are tested

The brief says to detect the cases with no valid lowering and fail loudly
rather than produce wrong output. An analysis that never says no is worth as
little as one that never finds anything, so `test_reject.py` provokes each
refusal by mutating a known-good input. All nine behave:

| provoked | refused because |
|---|---|
| RayQuery in a pixel shader | DispatchRays only launches raygen |
| two concurrent queries | TraceRay has one payload and one in-flight trace |
| groupshared memory | a raygen shader has no thread group |
| wave intrinsics | promotion to raygen changes lane occupancy |
| an unverified rayQuery opcode | refusing rather than guessing |
| committed state read inside the loop | the any-hit shader is a separate invocation |
| no Proceed loop and not FORCE_OPAQUE | no lowering is defined |

Opcodes are a **whitelist**. Only the nine this project has actually observed
in DXC output are accepted; anything else stops the lowering. Given that 184
and 185 share one LLVM function, a near miss here would render the wrong thing
silently, which is far worse than a refusal.

Writing that test found a real defect. The "no loop and not FORCE_OPAQUE" case
was accepted, because the check lived in `Query.pattern()` and `analyze()` never
called it, so a caller that did not ask for the pattern got a Query back for a
shader that cannot be lowered at all. Classification now happens during
analysis. The fix was to the code, not the test.

### What is generic and what is not

Generic: query discovery in any function, any number of resources of any type
read out of `!dx.resources`, `createHandle` to `createHandleForLib` for
arbitrary resource types including quoted template ones, arbitrary loop body
blocks, and metadata ids allocated above whatever the module already uses.

Not generic, and it matters:

- **The payload is fixed** at `{float, <2 x float>, i32}`, which carries
  exactly the three committed accessors the whitelist supports. Any other
  accessor is refused rather than silently dropped.
- **Root-level bindings only.** Descriptor tables, and `createHandleFromHeap`,
  are untested. This is the most likely thing to break on a real shader.
- **One entry point, one query.**
- Procedural primitives and the intersection shader path do not exist here at
  all; the candidate opcode for them is not in the whitelist, so such a shader
  is refused rather than mislowered.

## Descriptor tables: the thing flagged as most likely to break

Both the brief and this document named descriptor tables as the most likely
thing to break the rewriter on a real shader. Testing it showed that was the
wrong thing to worry about, and found the right one.

### A descriptor table alone changes nothing in the DXIL

`phase5/cases/rayquery_tablers.hlsl` is the opaque shader with an explicit
`[RootSignature]` using `DescriptorTable(CBV(b0)), DescriptorTable(SRV(t0)),
DescriptorTable(UAV(u0))`. Diffed against the root-descriptor original:

    IDENTICAL function body
    !6, !9, !12 resource records identical

The root signature lives in the container's `RTS0` part, not in the module. A
non-library shader reaches resources through `createHandle(rangeId, index)`
whatever the root signature says. So the rewriter is simply unaffected, and it
lowers and renders bit-exactly.

### The real hazard is resource ARRAYS, which tables enable

`phase5/cases/rayquery_table.hlsl` declares `RWStructuredBuffer<Result>
outBufs[4]` and writes through `outBufs[2]`. That does change the DXIL:

    %49 = call ... @dx.op.createHandle(i32 57, i8 1, i32 0, i32 2, i1 false)
    !9 = !{i32 0, [4 x %"class.RWStructuredBuffer<Result>"]* undef, ..., i32 4, ...}

The index operand is 2 rather than the 0 every shader tested so far had, and
the type is an array. **`_edit_resources` read the rangeId and ignored the
index entirely**, which is exactly the kind of silent wrong-render this project
is meant to avoid.

It did not render wrong, but only by luck: the resource record's regex failed to
match the array type and the lowering raised `LowerError`. Worse, the analysis
had already accepted the shader, and the CLI let the exception escape as a
Python traceback. A traceback is not "failing loudly" in any useful sense, so
`dxrewrite` now catches `LowerError` and reports it as a refusal.

### Supported rather than refused, because DXC showed the form

Rather than guess, `phase5/cases/reference/lib_array_ref.hlsl` asks DXC what a
LIBRARY does with a resource array:

    @outBufs = external constant [4 x %"class.RWStructuredBuffer<Result>"], align 4
    %2 = load %"class...", %"class..."* getelementptr inbounds (
             [4 x %"class..."], [4 x %"class..."]* @outBufs, i32 0, i32 2), align 4
    %3 = call %dx.types.Handle @"dx.op.createHandleForLib.class..."(i32 160, %"class..." %2)

The global carries the ARRAY type, the element is reached through a constant
`getelementptr`, and `createHandleForLib` takes the ELEMENT type. `lower.py`
now emits exactly that. Dynamic indexing and non-uniform indexing are refused,
since neither has been observed or tested.

### Verified end to end, with the decoy that makes it mean something

`raytest --table` binds the UAV through a real descriptor table of four
descriptors. Only slot 2 points at the output buffer; slots 0, 1 and 3 point at
a decoy. So a lowering that dropped the array index writes to the decoy and the
output comes back untouched.

    ground truth (WARP)                 14450 hits
    array lowering, descriptor table    14450 hits    MATCH, 0 mismatches

### The sensitivity check failed first, and it was the check that was wrong

The first attempt at poisoning the index reported **MATCH**, which should have
meant the test could not see a wrong descriptor at all. Diffing the two
libraries showed they were identical apart from a trailing newline: the
substitution looked for `@outBufs`, but the rewriter synthesises the global as
`@rq_uav0`, because a compute module's resource records carry empty name
strings and there is nothing else to name it after.

So the poison had never applied and the earlier "bit-exact" result was, at that
moment, worth less than it looked. Redone against the real symbol:

    index dropped to 0    65536 rays, 0 hits
    hit/miss mismatches: 14450    RESULT: DIVERGE

A separate control points the same way: the ROOT-DESCRIPTOR library run with
`--table` gives 0 hits, because it writes to u0, which is now the decoy. The
table path is real, it isolates, and it notices.

## An independently written shader, and the assumption it found

Everything up to here descended from the two Phase 2 shaders, which were
written to demonstrate one lowering. `phase5/cases/rayquery_indep.hlsl` is
deliberately not: different thread group (16x16), different control flow, the
query behind a helper function, `continue` inside the loop, the committed
accessors read in a different order, and, most importantly, **a resource read
inside the Proceed loop**, which is what a real alpha test does.

It keeps only the harness contract, a `Result` written to u0, so the diff still
works. WARP running the same shader is its own ground truth, so its alpha rule
does not have to match Phase 2's.

### It found a false refusal, and a consequential one

    REFUSED: Proceed loop body reads values defined outside it (%v2); the
    any-hit shader is a separate invocation and the payload is the only shared
    state

`%v2` is the `alphaMask` SRV handle, created in the entry block and used in the
loop. The isolation check was right in principle and wrong here: **a resource
handle is not caller state.** It names a resource, and every shader in the
library can reach the same one. The any-hit shader just creates its own.

This mattered. Refusing it would have blocked the most common real alpha test
there is, the one that samples a texture or buffer per candidate, which is the
whole reason alpha testing exists. The Phase 2 pair could never have exposed it,
because its alpha rule is pure arithmetic on the barycentrics.

The fix: `_check_loop_isolated` exempts `createHandle` results, and `_anyhit`
recreates each handle the body uses, **under the same SSA name it had in the
raygen**, so the transplanted instructions need no rewriting at all. Values
that are genuinely caller locals are still refused.

The generated any-hit reads exactly as it should:

    %rq.raw.v2 = load %"class.StructuredBuffer<float>", ...* @rq_srv1, align 4
    %v2 = call %dx.types.Handle @"dx.op.createHandleForLib..."(i32 160, ... %rq.raw.v2)
    %v41 = icmp eq i32 0, 0                      <- CandidateType tautology, folds
    ...
    %v49 = call ... @dx.op.rawBufferLoad.f32(i32 139, %dx.types.Handle %v2, ...)
    %v51 = fcmp fast ogt float %v50, 5.000000e-01
    br i1 %v51, label %bb52, label %rq.reject

### Result

    WARP RayQuery (its own ground truth)   65536 rays, 6333 hits
    automated lowering on the GTX 1070     65536 rays, 6333 hits
    0 mismatches, max |dt| and max |dbary| 0.000000

6333 is neither 14450 nor 8117, so the mask buffer is demonstrably being read
and is changing the outcome.

### Sensitivity, checked the way the last mistake taught

`raytest --cs <file.hlsl>` compiles a given shader for the ground-truth side, so
an independent shader can be its own oracle.

Inverting the mask comparison in the generated any-hit, `ogt` to `olt`, after
diffing to confirm the substitution actually applied this time:

    inverted   65536 rays, 8117 hits
    hit/miss mismatches: 14450    RESULT: DIVERGE

And 6333 + 8117 = 14450, the opaque hit count, so the accepted and rejected
sets partition it exactly. As before, neither number was arranged.

## The committed index accessors, and one that cannot be lowered at all

`phase5/cases/rayquery_ids.hlsl` is the second independent shader, using the
committed accessors the whitelist did not cover. In real code a hit is only the
beginning and these indices are what material data is looked up with, so they
were the obvious next thing a real engine would hit.

The refusal fired correctly, and named the numbers:

    UNSUPPORTED: unrecognised rayQuery opcode 207 ... This project has only
    verified 178, 179, 180, 182, 184, 185, 193, 194, 200.

DXC then supplied both halves, the way the recon established:

| RayQuery | | DXR 1.0 equivalent | |
|---|---|---|---|
| CommittedInstanceIndex | 207 | `InstanceIndex()` | 142 |
| CommittedGeometryIndex | 209 | `GeometryIndex()` | 213 |
| CommittedPrimitiveIndex | 210 | `PrimitiveIndex()` | 161 |

All three are `rayQuery_StateScalar.i32`, the same LLVM function as 184 and
185. That is now five opcodes sharing one function, and it keeps vindicating
the decision to dispatch on operand 0 rather than the callee name.

### CommittedGeometryIndex has no lowering on Tier 1.0

Adding all three produced a library the validator rejected:

    error: Flags must match usage.
    note: Flags declared=16, actual=33554448

33554448 is 0x2000010, so something set bit 25. Removing each opcode in turn
identified `geometryIndex` as the cause. Setting the flag to match let it
validate, and then the real answer arrived from the hardware:

    CreateStateObject (hr=0x80070057)    E_INVALIDARG, on the GTX 1070

**`GeometryIndex()` in a DXR 1.0 hit shader is itself a Tier 1.1 feature.**
There is nothing on this hardware to lower `CommittedGeometryIndex` onto. This
is the first Tier 1.1 feature found that this approach cannot emulate, and it
is worth stating plainly rather than filing as a limitation of the rewriter.

A route exists in principle: encode the geometry index in the shader table,
with one hit group record per geometry, and read it from the shader record.
That means the shim rebuilding the application's SBT, which is a much larger
change than anything here. Not attempted, and recorded so the option is not
lost.

The rewriter therefore recognises 209 specifically so it can explain itself,
and refuses it. It does not emit `dx.op.geometryIndex` at all, because doing so
would produce a library the driver rejects.

### The payload is part of the state object contract

Supporting the two that do work meant growing the payload from 16 to 28 bytes.
`CreateStateObject` then failed with `E_INVALIDARG` again, for a different
reason: the harness declared `MaxPayloadSizeInBytes = 16`.

Worth recording because it is a coupling, not a harness bug. **Whatever
generates the shaders does not get to choose the payload size freely**, it has
to agree with the state object's shader config. For a lowered compute shader
the shim creates that state object itself, so it controls both, but the two
must be changed together.

### Verified on a scene where the indices actually vary

Against the default scene, one triangle in one instance, every correct answer
is 0 and a lowering returning a constant would pass. So `raytest --multi`
builds two instances side by side, each a quad of two triangles: the left half
of the screen is instance 0 and the right half instance 1, and within each quad
the diagonal separates primitive 0 from primitive 1.

The shader packs both indices into the Result's float fields, so the existing
diff checks them.

    WARP RayQuery (ground truth)       65536 rays, 18496 hits
    automated lowering on the 1070     65536 rays, 18496 hits
    0 hit/miss mismatches, 0 value mismatches

### Sensitivity

Swapping the two payload fields the closest-hit writes, so instance and
primitive land in each other's slots, after diffing to confirm the edit applied:

    swapped    value mismatches: 9248    RESULT: DIVERGE

9248 is exactly half of 18496, which is what an even spread of the four
(instance, primitive) combinations predicts, since only the half where the two
differ is affected by a swap. That the number falls out at exactly half is
independent evidence the scene varies both indices as intended.

## Into the proxy: detection is in, the transform is a separate decision

Wiring the rewriter into the proxy splits into two halves that turned out to be
very different in size. The first is done; the second needs a decision that is
not mine to make silently.

### Detection, and why it costs almost nothing

The brief requires that a RayQuery shader be detected and reported clearly
rather than handed to a driver that cannot run it. That sounded like it needed
a bitcode parser inside the proxy. It does not.

MEASURED across every shader in `phase5`:

    plain compute shader        SFI0 = 0x0
    every RayQuery shader       SFI0 = 0x100000
    DXC-built DXR 1.0 library   SFI0 = 0x0
    the rewriter's own output   SFI0 = 0x0

`SFI0` is the container's Shader Feature Info part, eight bytes, and bit 20 is
the Tier 1.1 feature bit. So detection is a container walk and a mask, with no
LLVM involved. `proxy/dxil_scan.{h,cpp}` is the whole of it.

That the rewriter's OUTPUT reads 0x0 is worth noting on its own: the lowered
library correctly stops claiming Tier 1.1, which is what lets the driver accept
it.

Hooked in three places, since RayQuery can arrive by any of them:

- `CreateComputePipelineState`, the common case, because that is where a DXR
  1.1 engine puts RayQuery;
- `CreatePipelineState`, walking the subobject stream far enough to find a
  shader, and stopping at anything whose payload size is not knowable;
- `CreateStateObject`, checking each DXIL library, since RayQuery is legal in a
  raygen or miss shader too.

Detection does **not** change what is forwarded. On Tier 1.0 the driver rejects
these shaders anyway, and replacing its error with ours would hide information
without adding any. The log is the clarity.

Verified: `raytest` through the proxy reports the RayQuery compute shader by
its exact size, 4788 bytes. The DXR 1.0 probe and `D3D12RaytracingHelloWorld`
stay **silent**, so DXR 1.0 libraries do not false-positive, and the sample is
still pixel-identical at unchanged fps.

### The transform is a much larger commitment, and it is a fork

The rewriter is about 1100 lines of Python. The proxy is a C++ DLL loaded into
an application's process. There is no version of "call the Python from the DLL"
that is acceptable in a game's address space, so the transform cannot simply be
wired in the way detection was.

There is a second problem that is easy to miss. **The rewriter works on text,
and the proxy receives bitcode.** So a C++ port does not only need the analysis
and the lowering; it needs disassembly and assembly as well. Those exist, in
`dxcompiler.dll` (`IDxcCompiler::Disassemble`, `IDxcAssembler`) and `dxil.dll`
(signing, as Phase 1 established), which means **the shim would have to ship
and load DXC at runtime.** For a compatibility shim that is probably acceptable,
but it is a real deployment consequence and not an implementation detail.

The realistic options:

1. **Port the analysis and lowering to C++**, and load `dxcompiler.dll` plus
   `dxil.dll` from the proxy for the text conversion and signing. Honest, but
   comparable in size to all of Phase 5 so far, and adds two large runtime
   dependencies.
2. **Work on bitcode directly** in C++, avoiding the DXC dependency. Removes
   the shipping problem and reinstates the one the recon measured away: writing
   LLVM 3.7 bitcode. Strictly worse unless the DXC dependency is unacceptable.
3. **Keep the Python rewriter as an offline tool** and have the proxy substitute
   pre-lowered shaders by hash. This is the Phase 1 fallback, and its coverage
   is limited to shaders someone has already lowered, which does not meet the
   project's no-application-modification goal.

Option 1 is the only one that meets the goal, and it should be started with
open eyes about its size rather than drifted into.

### What this does NOT unblock

Reporting Tier 1.1 still cannot be turned on. Detection tells us when RayQuery
arrives; it does nothing about it. The tier flip and a working in-proxy
transform have to land together, exactly as the brief says, because a shim that
claims 1.1 and then fails is worse than one that claims 1.0.

## The C++ port, checked by byte-identical output

The fork above was decided in favour of porting. `proxy/rewriter/` is the
analysis and the lowering in C++, 1689 lines against the Python original's
1303, built into `phase5out/dxrw.exe` by `build_rewriter.bat`.

| C++ | ports |
|---|---|
| `ll_model.{h,cpp}` | `dxil.py` plus `llnorm.py`: parsing, CFG, dominators, natural loops, render |
| `rq_analyze.{h,cpp}` | `rayquery.py`: find, classify, refuse |
| `rq_lower.{h,cpp}` | `lower.py`: the transform |
| `phase5/dxrw.cpp` | the CLI, so the two can be compared |

### The bar: byte-identical, not "it renders correctly"

A port is exactly the situation where "the tests still pass" is too weak, since
both implementations are checked against the same six cases and a shared
misunderstanding would pass twice. So the bar is that the C++ must produce the
**same bytes** as the Python for every case:

    opaque    byte-identical (13533 bytes)
    alpha     byte-identical (14980 bytes)
    table     byte-identical (13675 bytes)
    tablers   byte-identical (13533 bytes)
    indep     byte-identical (16317 bytes)
    ids       byte-identical (13452 bytes)

The refusals agree too, message for message, including the two that matter
most: `CommittedGeometryIndex has no lowering on Tier 1.0` and `RayQuery in a
"ps" shader`.

This check is now part of `tools/run_rewriter_test.ps1`, so the two
implementations cannot drift apart quietly. It is demonstrably sensitive: it
failed on the first run, which is how the next item was found.

### What the byte comparison caught

The first run differed on every case, by exactly two blank lines. The cause was
in the PYTHON, not the port. `_append_shaders` located the end of the entry
function with `re.search(r'^\}\s*$', text, re.M)`, and in multiline mode `\s*`
also consumes the following newlines, so the insertion point drifted past them
and produced a run of blank lines nobody intended.

Both sides now do that insertion on lines instead. The generated IR is tidier,
and the accident is gone rather than faithfully reproduced in a second
language. Reproducing sloppiness in a port is not fidelity.

### Two things MSVC forced

- **`std::regex` has no multiline mode at all.** Every pattern that used one
  here was line-oriented anyway, so they became line operations, which is
  clearer than the regexes were. This is also what exposed the blank-line bug.
- **A raw string ending in `)"` terminates early.** `R"( ... !"(\w+)" ... )"`
  is not the string it looks like. Custom delimiters, `R"RX( ... )RX"`, where
  the pattern contains a quote.

### What this does NOT yet do

The port is the analysis and the lowering. It is not yet wired into the proxy,
and two pieces stand between:

1. **The DXC host.** The rewriter works on text; the proxy receives bitcode. A
   C++ path from container to text and back needs `IDxcCompiler::Disassemble`,
   `IDxcAssembler` and `dxil.dll` for signing, loaded from the proxy. That is
   the deployment consequence named in the fork, and it is not written yet.
2. **The dispatch path.** Lowering a compute shader to a library is only half
   of it. The application then calls `Dispatch`, which has to become
   `DispatchRays` against a state object and shader table the shim builds and
   owns. None of that exists.

So the tier flip is still blocked, for the same reason as before. What has
changed is that the transform itself is now in the language the proxy is
written in, and is provably the same transform.

## The DXC host: container in, signed container out

`proxy/rewriter/dxc_host.{h,cpp}` closes the gap between what the rewriter
works on and what D3D12 hands over. The rewriter takes `.ll` TEXT; the proxy
receives a DXIL CONTAINER.

    container -> text     IDxcCompiler::Disassemble   dxcompiler.dll
    text -> container     IDxcAssembler               dxcompiler.dll
    validate and sign     IDxcValidator               dxil.dll

The last step is the Phase 1 mechanism, unchanged: `dxil.dll` signs anything
that validates, with no secret key.

`dxrw rewrite <in.dxil> <out.dxil>` is the whole path in one call, and it is
what the proxy will do internally:

    rayquery_opaque   4788 -> 5932 bytes   pattern 1
    rayquery_alpha    5076 -> 6460 bytes   pattern 3
    rayquery_indep    5392 -> 6996 bytes   pattern 3
    rayquery_ids      4696 -> 5912 bytes   pattern 1
    rayquery_table    4796 -> 5948 bytes   pattern 1

All five render bit-exactly against WARP on the GTX 1070, and the container
path is now its own line in the regression, separate from the `.ll` path,
because it is the one that will actually run in an application.

### Three things a DLL in someone else's process has to get right

**Load by full path, never by name.** This is the one that would have been a
bad surprise. An application may already have its own `dxcompiler.dll` loaded,
and `LoadLibraryW(L"dxcompiler.dll")` would hand back THEIRS, of whatever
version. The host finds its own module, via `SetHostModule` from `DllMain`, and
loads the copies sitting beside it.

Proven rather than asserted. With the copy beside the exe hidden and a
DIFFERENT `dxcompiler.dll` present in the working directory:

    DXC unavailable: dxcompiler.dll is not next to the shim; the rewriter
    needs it to convert a DXIL container to text and back

It refused instead of silently loading the wrong one. (The first attempt at
this test was a no-op, because PowerShell's `Rename-Item` wants a bare name for
its destination and had quietly failed. The check only means something once it
has been seen to fail.)

**Never throw, never crash.** Every failure is a returned error. A missing DLL
is the message above, not a fault in the application's process.

**Load lazily and once.** Nothing is loaded until a shader actually needs
rewriting. `D3D12RaytracingHelloWorld` runs through the proxy with the whole
rewriter linked in and loads no DXC at all, still pixel-identical at unchanged
fps.

### What is left

The rewriter and its host are now linked into the proxy and the transform is
callable on what the proxy actually receives. **One piece still stands between
this and the tier flip**, and it is the larger one:

**The dispatch path.** Lowering a compute shader to a library is half the job.
The application then calls `Dispatch`, which has to become `DispatchRays`
against a state object and a shader table the shim builds and owns, with the
root signature and bindings carried across. Nothing of that exists. Until it
does, a rewritten shader has nowhere to run, which is why detection still only
logs.

## The dispatch path, and RayQuery running on Pascal

    RayQuery compute shader, unmodified, on a GTX 1070:

    opaque, numthreads(8,8,1)                      14450 hits   MATCH
    alpha-tested, generated any-hit                 8117 hits   MATCH
    independent, resource in loop, numthreads(16,16,1)  6333 hits   MATCH
    CommittedInstanceIndex + PrimitiveIndex, multi scene 18496 hits   MATCH

    0 mismatches throughout, against WARP running the same shader natively.

That is the project's goal reached: an application compiles RayQuery, creates a
compute pipeline and calls `Dispatch`, none of which can work on Tier 1.0
hardware, and it works.

### What the shim substitutes

`CheckFeatureSupport` reports Tier 1.1, so the application will emit RayQuery
at all. `proxy/rq_pipeline.{h,cpp}` does the rest:

| the application calls | the shim does |
|---|---|
| `CreateComputePipelineState` | lowers the DXIL, builds a state object and shader table, returns a stand-in |
| `SetPipelineState` | recognises the stand-in and remembers it, never forwarding it |
| `Dispatch(gx,gy,gz)` | `SetPipelineState1` then `DispatchRays` |

`Dxr11RayQueryPso` is an `ID3D12PipelineState` the application holds and never
inspects, standing in for machinery it knows nothing about. The same shape as
`Dxr11CommandSignature` in the indirect DispatchRays work, for the same reason.

### The thing that made this tractable

**DXR's global root signature IS the compute root signature.**
`SetComputeRootSignature` and `SetComputeRoot*View` are exactly what
`DispatchRays` consumes, so the application's bindings carry across with no
translation at all. The compute PSO's root signature becomes the state object's
`GLOBAL_ROOT_SIGNATURE` subobject and everything downstream just works. Most of
the feared difficulty here simply was not there.

### The one real conversion

`Dispatch` counts thread GROUPS; `DispatchRays` counts RAYS. The lowered raygen
reads `DispatchRaysIndex` where the original read `SV_DispatchThreadID`, which
is the global thread id, so the ray grid is the group count times the shader's
`numthreads`. That size is read out of the entry point's properties, tag 4,
before the lowering removes it.

The overhang is harmless: a shader that bounds-checked its threads bounds-checks
its rays identically. The 16x16 case exists in the regression precisely because
everything else is 8x8, and a hardcoded group size would have passed every
other test.

### The tier flip is OPT-IN, and the gate is tested

Reporting Tier 1.1 entitles an application to emit RayQuery, and the brief is
explicit that a shim which claims 1.1 and then fails is worse than one that
claims 1.0. So the flip is off unless `DXR11_TIER11=1`.

`.\tools\run_dispatch_test.ps1` checks the gate as a case of its own: without
the variable, the shim must still report Tier 1.0 and the same RayQuery shader
must still be refused. It is. Nothing about the default behaviour changed, and
`D3D12RaytracingHelloWorld` is still pixel-identical at unchanged fps.

A shader the rewriter refuses is logged and forwarded unchanged, so the
application gets the driver's own error rather than a silently wrong render.

## Measured against a real engine: the coverage gap

The next action was "a real application", because every shader proven so far is
one this project wrote for itself. Two things had to be established before that
could mean anything.

**No Microsoft sample uses RayQuery.** All eleven D3D12Raytracing samples are
DXR 1.0. The apparent matches for "inline" were the C++ keyword. So the samples
cannot exercise this at all, which is worth knowing: passing them proves the
shim is transparent, never that the rewriter is right.

**Unreal Engine 5.7 is the real RayQuery code available**, nine shader files
under `Engine/Shaders/Private`. They cannot be compiled standalone, since they
need UE's preprocessor and a great deal of engine plumbing, so this is a SURVEY
and not an execution test. It answers a narrower question honestly: what would
a shipping engine need that this project has not thought of?

### The answer: 8 of 25 accessors

(The survey first reported 24. Re-deriving the list by enumerating it gives 25,
an off-by-one carried through several entries below before it was caught. The
numbers here are the corrected ones.)

Counting distinct RayQuery accessors across Epic's Lumen and RayTracing
shaders, by frequency:

    CommittedRayT 11   CandidatePrimitiveIndex 11   CandidateInstanceIndex 11
    CandidateType 10   CommittedStatus 9            CandidateTriangleRayT 6
    CommittedInstanceID 4   CommitProceduralPrimitiveHit 4   Abort 4
    CommittedPrimitiveIndex 3   CommittedInstanceIndex 3
    ... and thirteen more at 1 or 2 uses each

The rewriter supports **eight**. So as things stand, **not one of Epic's
RayQuery shaders would lower.** That is the honest result, and it is far more
useful than another passing test on a shader written to pass.

### What is reassuring

**Every use is in a COMPUTE shader.** The brief calls the pixel-shader case the
most consequential of the no-lowering cases, since it is legal in DXR 1.1 and
some engines use it. Unreal does not. The one file that also contains
`[shader("anyhit")]` and `[shader("closesthit")]` holds separate entry points,
not a query inside a hit shader.

Epic's template flags are `RAY_FLAG_NONE`, `RAY_FLAG_FORCE_OPAQUE` and
`RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES`, all of which pass straight through.

### What the gap actually costs, measured not guessed

`GeometryIndex()` was the trap earlier: it looks like an ordinary hit-shader
intrinsic and is secretly Tier 1.1. So every proposed mapping was checked the
same way, by compiling it and reading the feature flags, rather than assumed.

| DXR 1.0 intrinsic | SFI0 | serves |
|---|---|---|
| `PrimitiveIndex`, `InstanceIndex`, `InstanceID`, `HitKind`, `RayTCurrent`, `ObjectRayOrigin`, `ObjectRayDirection`, `WorldToObject4x3` | **0x0** | 11 of the missing accessors |
| `GeometryIndex` | **0x100000** | Tier 1.1 only, blocked |
| `InstanceContributionToHitGroupIndex` | does not exist in HLSL | blocked |

So the sixteen unsupported accessors split:

- **11 mechanically addable.** Each maps to an intrinsic measured to be plain
  DXR 1.0. There is an elegance here worth recording: the SAME intrinsic serves
  the Candidate and Committed forms, because in an any-hit shader
  `PrimitiveIndex()` IS the candidate and in a closest-hit it IS the committed
  hit. One table, and which generated shader the read lands in decides the
  meaning.
- **2 blocked as Tier 1.1**: Candidate and CommittedGeometryIndex, the case
  already documented.
- **2 blocked with no intrinsic at all**:
  `*InstanceContributionToHitGroupIndex`. HLSL does not expose it to a hit
  shader in any form.
- **2 structurally larger**: `CommitProceduralPrimitiveHit`, which needs the
  generated intersection shader this project has never built, and `Abort()`,
  which the brief maps to `AcceptHitAndEndSearch()` or a payload flag.

### One thing this survey cannot settle

Several of Epic's files declare more than one `RayQuery` object,
`RayTracingReflectionReorientedParticleMaterialCS.usf` declares five. Whether
those are concurrent within one entry point after inlining, which the rewriter
refuses, or merely separate functions, which is fine, cannot be determined by
reading the source. It needs the compiled DXIL, and compiling it needs the
engine.

## The 11 accessors: coverage 8 of 25 becomes 19 of 25

The survey said 11 of the 16 gaps were mechanically addable. They are added,
in both implementations, and they work.

| RayQuery | | DXR 1.0 | |
|---|---|---|---|
| CandidateInstanceIndex | 201 | `instanceIndex` | 142 |
| CandidateInstanceID | 202 | `instanceID` | 141 |
| CandidatePrimitiveIndex | 204 | `primitiveIndex` | 161 |
| CandidateTriangleRayT | 199 | `rayTCurrent` | 154 |
| CandidateObjectRayOrigin | 205 | `objectRayOrigin` | 149 |
| CandidateObjectRayDirection | 206 | `objectRayDirection` | 150 |
| CandidateWorldToObject | 187 | `worldToObject` | 152 |
| CandidateTriangleFrontFace | 191 | `hitKind` == 254 | 143 |
| CommittedInstanceID | 208 | `instanceID`, via payload | 141 |
| CommittedTriangleFrontFace | 192 | `hitKind`, via payload | 143 |
| CommittedWorldToObject | 189 | `worldToObject`, via payload | 152 |

The operand shape is IDENTICAL on both sides apart from the query handle, which
simply goes away. `StateMatrix(op, handle, i32 row, i8 col)` becomes
`worldToObject(152, i32 row, i8 col)`. That uniformity is why eleven accessors
cost one small table rather than eleven special cases.

Two do need conversion. `*TriangleFrontFace` returns `i1` in RayQuery while
`HitKind()` is an integer, so it becomes `icmp eq i32 hk, 254`. And the
world-to-object matrix is twelve floats, so it travels in the payload as
`[12 x float]` indexed `row * 4 + col`.

### The payload: fixed layout, conditional cost

The payload grows from 28 to 84 bytes, almost all of it the matrix. The LAYOUT
is fixed even when the matrix is unread, because variable field offsets across
two implementations is an excellent way to get one of them subtly wrong. What
is made conditional is the per-invocation COST: the closest-hit emits its
twelve fetches and twelve stores only when the shader actually reads the
matrix. Memory is the cheap axis here; work per hit is not.

`MaxPayloadSizeInBytes` had to move with it in two places, the harness and
`rq_pipeline.cpp`. That coupling was already documented and it bit exactly as
predicted.

### Two bugs the work exposed

**The loop isolation check was blind to phi nodes.** The first version of the
test shader accumulated a value across candidates and read it after the loop,
which cannot be lowered. The rewriter did not refuse it; it produced IR the
assembler rejected with `use of undefined value '%v64'`.

The cause: `Uses()` collected operands from call arguments only, and a phi is
not a call. **A phi is exactly how a value escapes a loop**, so the check was
blind to the one shape it exists to catch. Fixed in both implementations, and
the shader is now correctly refused with "value %v64 defined in the Proceed
loop is used after it". The test shader was rewritten to do what a real alpha
test does: let the candidate accessors decide the commit and nothing else.

**A refusal test quietly stopped testing anything.** `test_reject.py` used
opcode 191 as its example of something unverified. The Unreal survey turned 191
into `CandidateTriangleFrontFace`, so the "unverified" case became a verified
one and the check started accepting what it was written to refuse. It now uses
250, which is genuinely unobserved, with a comment saying why.

That is the second time in this project a test has decayed rather than failed.
Worth the general lesson: a test whose premise is a fact about the world needs
re-checking when that fact changes.

### Results

    seven shaders lowered, C++ and Python BYTE-IDENTICAL on every one
    acc, the new accessor shader     18496 hits   MATCH, 0 mismatches
    12 of 12 refusals behaved
    five shaders end to end through the proxy on the 1070, all MATCH

Coverage against Unreal's 25 accessors goes from 8 to 19. What remains is the
four permanently blocked (geometry index and instance contribution to hit group
index) and `CommitProceduralPrimitiveHit` plus `Abort()`.

## Abort(): 20 of 25, and a limit on what can be verified

`Abort()` is opcode 181. The brief offers two mappings, and only one of them is
exact, which is the interesting part.

A DXR 1.0 any-hit shader has exactly two terminators: `IgnoreHit()`, which
rejects the candidate and CONTINUES traversal, and `AcceptHitAndEndSearch()`,
which accepts it and stops. **There is no "reject and stop"**, and that is
precisely what a bare `Abort()` needs. So `AcceptHitAndEndSearch` is exact only
for the commit-then-abort shape, and covers nothing else.

The payload flag covers both, uniformly:

    payload gains `aborted`; Abort() stores 1
    the any-hit prologue ignores its candidate at once if the flag is set

Commit-then-abort still accepts, because the control flow still falls through.
A bare abort still rejects. In both cases nothing further is committed, which
is what stopping traversal means for the RESULT. Traversal itself carries on,
so this is slower than the ideal; `AcceptHitAndEndSearch` would be exact for
the commit case, but only after proving the commit dominates the abort in the
same iteration, and correctness comes before that.

The generated any-hit reads exactly as intended:

    %rq.abv = load i32, i32* %rq.pab
    %rq.abc = icmp ne i32 %rq.abv, 0
    br i1 %rq.abc, label %rq.reject, label %rq.body
    ...
    bb13:  store i32 1, i32* %rq.pab   ; commit then abort -> accept
           br label %rq.accept
    bb16:  store i32 1, i32* %rq.pab   ; bare abort -> reject
           br label %rq.reject

### What CANNOT be verified, and why

**`Abort()` is inherently order-dependent.** Its whole effect is to stop at
whichever candidate traversal happens to reach first, and that order is
implementation-defined. WARP and NVIDIA may legitimately visit in different
orders, so anything order-dependent, the committed t, the barycentrics, which
instance was hit, **cannot be compared between them at all**. No test of those
would be meaningful, and one that appeared to pass would be luck.

So the test observes only WHETHER there was a hit, which with
commit-then-abort is order-independent: a hit occurs exactly when some
candidate passes, whatever order they are seen in.

    WARP RayQuery (ground truth)      65536 rays, 10377 hits
    lowered, on the GTX 1070          65536 rays, 10377 hits   MATCH

That still has teeth. Pre-setting the abort flag, so every candidate is ignored:

    abort flag pre-set    65536 rays, 0 hits
    hit/miss mismatches: 10377    RESULT: DIVERGE

So the flag, its initialisation and the prologue check are all exercised. What
is NOT established is the TIMING, that traversal stops at the right candidate,
and no comparison against WARP can establish it. Stated here rather than left
for someone to assume the green tick covers it.

### Where that leaves coverage

Twenty of Unreal's 25 accessors. What remains is
`CommitProceduralPrimitiveHit`, needing the generated intersection shader, and
the four that are permanently refused.

    eight shaders lowered, C++ and Python BYTE-IDENTICAL on every one
    six shaders end to end through the proxy on the 1070, all MATCH
    12 of 12 refusals behaved

## Procedural primitives: the last lowering, and a real obstacle beside it

    procedural-only RayQuery on the GTX 1070   7396 hits   MATCH, 0 mismatches

`CommitProceduralPrimitiveHit` is 183, `CandidateProceduralPrimitiveNonOpaque`
is 190. On the DXR 1.0 side `reportHit` is 158, overloaded on the attribute
struct, and an intersection shader is shader kind 8 carrying neither a payload
size nor an attribute size.

### One body, two substitutions, two shaders

The lowering turned out to be the prettiest part of the project. The
intersection shader is **the same loop body as the any-hit**, with one
substitution changed:

    any-hit        CandidateType() folds to 0   CANDIDATE_NON_OPAQUE_TRIANGLE
    intersection   CandidateType() folds to 1   CANDIDATE_PROCEDURAL_PRIMITIVE

The triangle branch then dies and the procedural branch survives, or the other
way round. `CommitProceduralPrimitiveHit(t)` becomes `ReportHit(t, 0, attrs)`,
and every way out of the loop body becomes a plain `ret void`, because an
intersection shader has no accept or reject terminator: reporting IS accepting,
and returning reports nothing.

The generated shader is the slab test transplanted verbatim, with
`CandidateType` folded to a constant-true `icmp eq i32 1, 1` and the object-space
ray accessors becoming ordinary DXR 1.0 intrinsics. The closest-hit also writes
status 2, `COMMITTED_PROCEDURAL_PRIMITIVE_HIT`, rather than 1.

### The obstacle, which is not in the lowering

**A query that commits BOTH triangle and procedural hits is refused**, and this
is not laziness. A hit group is either triangles or procedural, never both, and
which one a geometry uses is chosen by `InstanceContributionToHitGroupIndex`,
which the APPLICATION set when it built its acceleration structures. A BLAS
also carries only one geometry type, so the two always live in different
instances.

To build a correct shader table for a mixed scene the shim would need to know,
for every instance, which BLAS it points at and what geometry type that BLAS
holds. That information exists only in the
`BuildRaytracingAccelerationStructure` calls, so supporting it means
intercepting every one of them and tracking the geometry type of every BLAS.
The shim does not do that, so it refuses with a message saying exactly this.

Epic's shaders do mix, so this is the difference between procedural working and
procedural being useful. It is the largest single piece of work left.

**A related limitation that applies TODAY, including to triangles.** The shim
builds one hit group record and dispatches with
`RayContributionToHitGroupIndex`, `MultiplierForGeometryContributionToShaderIndex`
and `MissShaderIndex` all zero, so every geometry resolves to record 0. That is
correct only while every instance has `InstanceContributionToHitGroupIndex = 0`.
An application that set it for its own DXR 1.0 use would index past the single
record. Not yet handled, and it needs the same AS interception.

### Sensitivity

Reporting a constant `t` of 1.5 instead of the computed 2.0:

    7396 value mismatches, max |dt| 0.500000    RESULT: DIVERGE

And the mixed case refuses, as its own test case.

### A divergence byte-identity could not see

Porting this exposed something worth recording. The C++ any-hit generation had
silently lost the Python's "loop body branches outside the loop" refusal during
the original port. **Byte-identity never noticed, because it compares OUTPUTS
and a missing refusal produces no output to differ.** The two implementations
emitted the same bytes for everything tested while disagreeing about what to
reject.

That is the second limit found in this project's strongest oracle, after "it
proves agreement, not correctness". It also proves agreement only where both
produce something.

## Acceleration structure interception: the cheap half

`proxy/as_tracker.{h,cpp}` is the first half of what the shader table needs.
The two halves turned out to be very different in cost, which is the finding.

Recall why any of this is needed. A record in the shader table has to match the
geometry that resolves to it:

    index = RayContributionToHitGroupIndex
          + MultiplierForGeometryContributionToShaderIndex * GeometryIndex
          + InstanceContributionToHitGroupIndex

The first two are the shim's, in the `DispatchRays` call. The third is the
APPLICATION'S, baked into the instance descriptions when it built its top-level
structure.

### BLAS geometry types are free

`D3D12_RAYTRACING_GEOMETRY_DESC` arrives as **CPU memory** in the build call, so
the geometry type of every bottom-level structure can simply be read and
remembered. No copy, no sync, no cost. `ARRAY_OF_POINTERS` is handled as well as
`ARRAY`; anything else is left `kUnknown` rather than guessed at.

Verified on the Phase 4 probe, which builds one of each:

    bottom-level AS at 0x9185000: 1 geometry, triangles
    bottom-level AS at 0x9435000: 1 geometry, procedural AABBs
    top-level AS build seen, 2 instances. ...

and it distinguishes correctly rather than always saying the same thing:

    raytest triangle scene     1 geometry, triangles
    raytest --proc scene       1 geometry, procedural AABBs

### TLAS instance data is NOT free, and getting it was the work

`InstanceDescs` is a **GPU virtual address**, and a copy needs an
`ID3D12Resource`. **D3D12 has no API that turns an address back into a
resource.** That is the whole obstacle, and it is why this half cost what the
other did not.

The way through: every resource the application creates goes through the
wrapped device, so the shim can remember the mapping itself.
`proxy/res_tracker.{h,cpp}` keeps buffers in a map keyed by start address, and
`Find` answers with the resource and the offset within it. Two decisions there,
both about lifetime and both deliberate:

- **No reference is held.** An `AddRef` would change when the application's
  resources die, which a transparent shim must not do, and would leak for every
  buffer an application ever creates. Entries can go stale; that is safe for the
  one use there is, because a lookup only happens while the application is
  passing that buffer to a build, so it must still own it.
- **An entry is replaced** when a new resource reports the same start address,
  which is how address reuse after a free is handled.

Measured on the Phase 4 probe, which is the first thing to check because if the
lookup fails the whole approach is dead:

    top-level AS build seen, 2 instances. Instance buffer at 0x9184000
    RESOLVED to resource 000002465ECD9650 + 0x0, out of 7 tracked buffers

### Two paths to the data, and only one of them costs anything

Once the resource is named, reading it splits the same way the indirect
`DispatchRays` arguments did.

**CPU-visible (upload heap): free.** The application wrote the descriptions
from the CPU, so they are simply mapped and read at record time. No copy, no
fence, no stall. This is what both Microsoft samples and every test scene do.

**GPU-only (default heap): a copy and a fence, but no stall.** A
`CopyBufferRegion` into a readback buffer is recorded into the application's own
list, wrapped in `NON_PIXEL_SHADER_RESOURCE` to `COPY_SOURCE` and back, which is
the state DXR requires that buffer to be in at a build. The queue hook then
signals a fence after the submission and parses the result on a **later**
submission, whenever the GPU has passed it. Nothing ever waits. The answer
arrives a submission late, which is acceptable because nothing needs it the
instant it is recorded.

The reading is done **once per destination address**. An engine rebuilds its
top-level structure every frame into the same memory, and a copy per frame for
an answer that does not change is not worth paying. The cost of that choice: an
application that changes its contributions in place keeps the first answer.

Not read: `ARRAY_OF_POINTERS` instance descriptions, where each pointer is
itself a GPU address needing a second dependent copy. Logged, not guessed at.

`-gpuinst` on the Phase 4 probe puts the descriptions in GPU-only memory
specifically so the expensive path is exercised rather than hypothetical. Both
paths produce the same answer, with the debug layer on and silent.

### What it found, and it is exactly what was predicted

The probe scene, read through the shim on the 1070:

    top-level AS at 0x9437000 READ: 2 instances, max
    InstanceContributionToHitGroupIndex 1, geometry reached: triangles and
    procedural, 0 instance(s) pointing at an unseen bottom-level structure

which matches `phase4/tier11probe.cpp` exactly: two instances with
contributions 0 and 1, instance 0 on the triangle BLAS and instance 1 on the
procedural one. Both open problems in one scene, now measured rather than
assumed.

The Microsoft samples read as 1 instance, max contribution 0, triangles only,
which is the case the shim's single-record table is already right for.

### The refusal this bought

The point of knowing is not to log it. A lowered RayQuery dispatch builds ONE
hit group record of ONE geometry type, and until now it would have run anyway
on a scene where that is wrong, producing a quietly incorrect image. It now
refuses:

    lowered RayQuery dispatch REFUSED: the scene uses nonzero
    InstanceContributionToHitGroupIndex, so a single hit group record would not
    be the one the ray resolves to. Nothing is drawn for it.

Shown to be real rather than unreachable with `raytest --multi --contrib`,
which gives the two instances different contributions: WARP finds 18496 hits,
and the shim declines to answer instead of inventing a number.

Two limits of that check, stated because they are real:

- It is judged over **every** top-level structure read, not the one the shader
  is about to trace against, because which structure that is is not knowable at
  the dispatch when it arrives through a descriptor table. So it can refuse a
  dispatch that would have been fine. Refusing is the safe direction.
- It can only see what has been **read**. On the GPU-only path the answer
  arrives a submission late, so the first dispatch of a run is not covered.

### Regression

Unchanged by all of this: 7 of 7 end-to-end dispatch cases MATCH against WARP,
the probe on hardware still gives 14450 + 2312 + 48774 = 65536 on both
argument-buffer shapes, `D3D12RaytracingHelloWorld` is 0 of 14400 pixels
different, `D3D12RaytracingSimpleLighting` runs at baseline fps, and the debug
layer is silent through the new barriers and copies.

## Next

1. **Use the instance data.** Size the shader table by the maximum contribution
   and place a record of the right type at each index. That turns both refusals
   into working scenes, and it is the last structural piece of the shim.
2. **Dynamic descriptor indexing**, still refused.
3. **Only then consider making the tier flip the default**, once enough real
   software has run through it that the refusal list is trusted.
