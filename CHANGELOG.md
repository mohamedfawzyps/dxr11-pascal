# Changelog

## Versioning

[Semantic Versioning](https://semver.org/). The major version is the honest
part:

- **0.x** until real software runs through it end to end. A real Unreal
  shader first lowered in 0.25.0, but no real game has
  yet run with its RayQuery shaders lowered and dispatching, so the boundary
  between "runs" and "refused" is measured and not yet trusted.
- **1.0.0** when real software has run through it and that boundary holds.
  That is a statement about exposure, not about features.

MINOR adds lowering coverage or a feature. PATCH is fixes only. The version is
compiled into the DLL and logged on attach, so a log file identifies its own
build.

**Tags.** A release is tagged `vX.Y.Z` on the commit that completes it,
documentation included. Not every version has one: 0.15.0 through 0.24.0 and
the patch series 0.36.1 through 0.36.7 were never tagged and exist only as
commits on `main`. This file is the complete list of versions; `git tag` is
not.

---

## 0.55.0

**The scene a `GeometryIndex()` dispatch traces is resolved through the
RAYGEN's local root signature** (Tier 1.1 item a, "a scene through a local
root signature").

- **The gap.** The shim found the scene only through the global root
  signature. One bound in the raygen's shader record was not resolved, so the
  dispatch was judged over every live scene: right with one, refused when two
  disagree, and never served by the variant, which needs the exact scene.
- **Measured on 0.54.0 first**, with the new `gitest --localscene` (a root SRV
  at t0 space2 in the raygen's record) and `--localscenetable` (a descriptor
  table there), a conflicting decoy in the global root SRV: all 7 layouts
  refused, "hit group record 2 is reached by two different geometries".
- **The fix.** When a TraceRay's register is not in the global signature, the
  raygen being dispatched is found by the identifier its record starts with,
  its local root signature by the spec's association rules (lifted out of the
  variant's rebuild into `Assoc`, followed into renamed collections), and the
  scene's address or descriptor table read from the record, as is a heap index
  in its root constants or root CBV. Where the record is read, by cost:
  - in CPU-visible memory, at record time, no wait;
  - in GPU memory, a copy of the record by address (`proxy/shim_copy.hlsl`)
    recorded before the dispatch, which waits for submit and joins the one
    wait a deferred dispatch already costs;
  - in GPU memory with GPU-written arguments, whose record address is known
    only at submit: one extra copy and wait there, logged once.
  A build of the scene recorded after the dispatch but before its submit is
  NOT DRAWN, by name.
- **Only at recursion depth 1**, where only the raygen traces. Deeper, a
  closest-hit's or miss's own local root signature could name another scene,
  so that stays judged over every live scene, by name; it belongs to the next
  gap, TraceRay in a closest-hit or miss.
- **Measured:** the local scene cases match WARP in both forms, with the
  shader table and the arguments in CPU or GPU memory, instances GPU-written
  or stale, through collections and AddToStateObject, at 6.5 and 6.6. The
  matrix, now `tools/run_gitest_matrix.ps1`, 122 of 122 (46 before). Dispatch
  suite 47 of 47 and every gate.

## 0.54.0

**A `GeometryIndex()` dispatch on a scene not read from its latest build is
deferred to submit, and the variant no longer serves such a scene blind. It
drew a deserialized bottom-level structure WRONG, silently** (Tier 1.1 item a,
the gap named in 0.53.0).

- **The gap.** Since 0.51.0 a dispatch whose scene was unread or stale went to
  the variant, which builds its table on the GPU from the saved instances and
  never learns which bottom-level structures they point at. Its layout gives
  each instance `gmax` records per trace, the most geometries of any structure
  the shim knows. A deserialized structure's count is unknown, so one holding
  more spilled into the next instance's records.
- **Measured on 0.53.1 first**, with the new `gitest --deserialize`: every A
  instance on a deserialized copy of A (4 geometries), and A's own address
  rebuilt with 2, as a streaming engine reuses memory. With CPU-visible
  instances refused by name, as intended. With GPU-written instances, **all 7
  layouts drawn wrong, 8192 to 16384 pixels each, nothing logged**.
- **The fix.** A dispatch whose scene resolves exactly and is not read from its
  latest build waits for submit, as a RayQuery dispatch has since 0.52.0, direct
  and CPU-visible indirect alike (`QueueStaleRays`); an indirect one from GPU
  memory already did, and now also records its build. At the split the build
  has run and is read exactly (`astrack::BringToBuild`), so an unknown structure
  is refused by name before the variant is considered. The variant now serves
  only records several geometries share, on a read of the latest build, where
  every instance's structure is known and `gmax` covers it. A scene built again
  after the dispatch was recorded but before it was submitted is NOT DRAWN, by
  name. An unresolved scene that is stale is not drawn either; the variant
  never served one.
- **Measured:** `--deserialize` refused by name with either instance kind;
  `--gpuinst` and `--stale` 7 of 7 through the deferral; the gitest matrix 46
  of 46; dispatch suite 47 of 47 and every gate, with a new gate for the
  deserialize case, checked to fail on 0.53.1.
- **Cost:** a `GeometryIndex()` dispatch on a GPU-built scene now waits at
  submit for its segment to run. In Unreal that is the ray tracing debug view.

## 0.53.1

**An instance with a NULL bottom-level structure is inactive, not unknown.
0.53.0 refused the scene for one, 30 dispatches in Escher.**

- **In the game (0.53.0, 3 min 12 s):** clean end marker, 162 RayQuery
  shaders lowered, every `CreateStateObject` hr=0, 11663 lowered dispatches
  drawn, and 30 REFUSED, all "unknown bottom-level structure", one instance,
  from 2 minutes in.
- **Read from Unreal's source:** `RayTracingInstanceBufferUtil.usf` writes a
  culled instance, and one with an invalid transform, with a structure address
  of 0. **Read from the spec** ("Inactive primitives and instances"): an
  instance with a NULL bottom-level pointer is legal but inactive, discarded at
  build. The shim counted it as a structure it had never seen, which 0.53.0
  turned into a refusal. Before that it was counted silently. INFERRED, not
  proven, that this was the game's instance: the log did not say which one.
- **The fix:** an instance whose structure address is 0 reaches no record and
  is skipped. A refusal for a structure of unknown geometry now NAMES it and
  why: never seen, deserialized, a copy of something unknown, built with no
  readable geometry, or its first build recorded after the top-level build
  that uses it.
- **Measured:** `raytest --nullinst` adds an instance with a null structure
  and contribution 7. 0.53.0 refuses it. WARP access-violates on one
  (`d3d10warp.dll`), as it divides by zero on an empty scene, so the oracle is
  the definition: the scene WITHOUT that instance, on WARP. 0.53.1 matches it,
  13872 hits, with CPU-visible and GPU-written instances. New suite gate; 47
  of 47 and every gate.

## 0.53.0

**Structures made by `CopyRaytracingAccelerationStructure` are followed
(Tier 1.1 item a, the gap listed as a copied top-level structure). And it was
worse than listed: a copied BOTTOM-level structure was drawn WRONG,
silently.**

- **Why it matters for Unreal.** It COMPACTs bottom-level structures
  (`D3D12RayTracing.cpp:5176`) and loads offline ones by DESERIALIZE
  (`:4339`). The shim learns a structure only from its build, so a copy was
  unknown: an instance on one counted one geometry of unknown kind and was
  left out of the table, and a copy over a known address kept the old
  answer. Escher's runs so far showed no instance on an unseen structure.
- **Measured on 0.52.3 first**, with new `raytest` modes: `--blasclone`
  (the instance on a CLONE of the bottom-level structure) **drawn with 0
  hits of 13872, nothing logged**, with CPU-visible or GPU-written instances;
  `--deserialize` (serialized, then deserialized) the same; `--tlasclone`
  (the dispatch traces a CLONE of the top-level structure) refused.
- **The fix.** CLONE and COMPACT make the same structure: a bottom-level
  copy keeps the source's geometry, a top-level copy the source's instances
  as of its latest build, a read still pending there followed at the split
  (an alias in `astrack::BringToBuild`), and the source's saved instances
  and scene copy stand for it in `shimscene`. Any other copy into an address
  leaves nothing known there.
- **Refused by name:** an instance on a bottom-level structure of unknown
  geometry, a deserialized one above all (its geometry is in a driver-opaque
  blob), in the RayQuery path and the `GeometryIndex()` table path. The
  `stats:` line counts it ("unknown bottom-level structure"). Not refused:
  the `GeometryIndex()` VARIANT, used when the read is stale, cannot see the
  instances on the CPU, so an unknown structure there is a named gap.
- **Measured:** the three copy cases match on the 1070 and deserialize is
  refused with its reason. Suite cases `blasclone`, `blasclonegpu`,
  `tlasclonegpu` and a gate for the deserialize refusal: 47 of 47 and every
  gate. gitest 46 of 46.

## 0.52.3

**FIXED: a lowered RayQuery dispatch drawn WRONG, silently, when a
bottom-level address was reused after it.** Found from the 0.52.2 Escher run.

- **In the game at 0.52.2 (2026-09-25, 5 min 18 s):** clean end marker,
  13689 lowered dispatches drawn, 0 "scene not read yet" (0.52.2's fix
  held). But **1190 refused in about 40 s** (09:32:11 to 09:32:52) as "one
  structure disagrees": two instances with different (contribution,
  geometry) pairs on hit group record 1090. Unreal assigns contributions as
  a running sum of geometry counts, so its instances should not collide.
- **The cause, reproduced offline before any change.** A top-level build's
  instances say which bottom-level structure each points at, by ADDRESS;
  the shim takes that structure's geometry count from its build. Since
  0.52.0 GPU-written instances are parsed at submit, and it took the LATEST
  build recorded at that address. An engine streaming geometry records the
  next frame ahead of the GPU and reuses addresses, so the count could come
  from a structure the dispatch never traced. `raytest --blasreuse` (new)
  rebuilds the structure at the same address with 1 geometry instead of 4,
  after the dispatch, in a list submitted after it: 0.52.2 drew it from a
  3-record table where 6 are reached, 4624 hits against 13872, **9248
  mismatches, nothing logged.** Too FEW geometries draws wrong; too many
  makes false collisions like record 1090. (That the Escher refusals were
  this is inferred, not shown: the log names only the record.)
- **The fix.** Every structure build gets a serial in recorded order; each
  bottom-level address keeps the builds it has had, pruned only past what a
  not-yet-parsed top-level build could need; a top-level build's instances
  are parsed against the bottom-level structures as of that build, which is
  what the CPU-visible path always saw. The `GeometryIndex()` variant's
  record blocks likewise never use fewer geometries than the largest known
  when the build was recorded.
- **The refusal now names who collides:** the pair on the record, the
  instance bringing the other, its structure and geometry count.
- Suite case `blasreuse`: 44 of 44 and every gate. gitest 46 of 46; the
  Phase 4 probe matches in every mode. Not covered by its own test: the
  variant's geometry floor.

## 0.52.2

**IN THE GAME at 0.52.1 (2026-09-25, 2 min 21 s, debug layer forced on):**
clean end marker, 1680 lowered dispatches drawn (all indirect), every scene
resolved, and the two debug layer errors 0.52.1 fixed are gone (only
Unreal's 24 native 16-bit shaders remain). ONE dispatch refused at submit,
and it found two gaps, both fixed here:

- **An EMPTY scene was never read, so a dispatch on it drew nothing.** The
  first frame of Escher's open world builds its top-level structure with 0
  instances, which has nothing to copy and so no read; the dispatch was
  refused. The right answer needs no read: every ray misses. Such a build is
  now known at once (`astrack::NoteEmpty`). This was so with CPU-visible
  instances too, and was the one "scene not read yet" refusal of every
  Escher run since 0.49.0. **WARP crashes tracing an empty scene** (divide
  by zero, RayQuery and TraceRay alike), so the check is the definition:
  `raytest --empty` (new) with `--prefill`, on the 1070, must give 0 hits.
  0.52.1 gave 49154, the prefill left behind; 0.52.2 gives 0.
- **A build in one list and the dispatch in another, submitted in ONE
  ExecuteCommandLists call.** The build list's read was stamped only after
  the whole call, so at the dispatch's split it could not be known to have
  run, and the dispatch was refused. Each list's reads are stamped as it is
  submitted now. `raytest --sameecl` (new) records the in-place rebuild into
  a second list submitted with the dispatch: 9248 mismatches on 0.52.1,
  direct and indirect, a match on 0.52.2. Not hit in Escher.
- Suite cases `sameecl` and `sameeclind`, and a gate for the empty scene
  with both kinds of instances: 43 of 43 and every gate. gitest 46 of 46;
  the Phase 4 probe matches in every mode.

## 0.52.1

Two D3D12 debug layer errors the shim caused, found in an Escher run
(2026-09-25, 2 min 31 s, the layer forced on; that run was still shim
0.49.0, the newer DLL not copied in). Neither changed what was drawn.

- **#1161, every lowered RayQuery dispatch that reads record data:** the
  miss table was declared at the HIT records' stride, 64 bytes, around a
  32-byte record. Invalid per the API; the 1070 ran it correctly because
  the lowered raygen only ever uses miss index 0. 1731 errors, one per
  dispatch drawn. The miss table now has its own 32-byte stride. Reproduced
  offline first with `DXR_TIER11_DEBUGLAYER=1 raytest ... --geom --contrib`,
  and gone after.
- **#901, 5 times:** the resource tracker asked every new buffer for its
  heap, reserved ones included, where D3D12 does not allow the question.
  Reserved buffers are now recorded as GPU-only without asking.
- Everything else the layer said in that run is Unreal's own: duplicate
  barrier descriptors (the shim records only single ones), overlapping
  resources at one address, zero-group dispatches, and the 24 native 16-bit
  compute shaders known since 0.36.
- Dispatch suite 41 of 41; gitest `--indirectgpu` and `--stale --sm66` match.

## 0.52.0

**FIXED: a lowered RayQuery dispatch after the scene changed was drawn from
the OLDER build's table, silently.** The gap 0.51.0 found in the
`GeometryIndex()` table, in the RayQuery path, which is what Escher draws.

- **Measured first.** `raytest --gpuinst` (new) puts every top-level build's
  instances in GPU memory, copied there on the GPU, as Unreal writes them.
  `--geom --contrib --rebuild --gpuinst` rebuilds the scene in place with a
  new layout: 0.51.0 drew the second dispatch from the first build's table,
  **9248 hit/miss and 4624 value mismatches, no refusal logged.** The same
  case with upload-heap instances matches. `--multi --contrib --rebuild
  --gpuinst` also drew from the older table (1 record where 2 are reached)
  and matched only because that shader never reads the contribution.
- **Why.** GPU-written instances were copied out every 8 builds of a
  structure and parsed a submission late, so at a dispatch what the shim knew
  was usually an older build's scene. For a scene that never changes the two
  agree, which is why nothing showed.
- **The fix.** Every such build's instances are read now, by the 0.51.0 save
  pass plus a copy into a readback buffer, and each read carries its build.
  A dispatch remembers which build of its scene it traces. A direct one
  whose scene is not read from that build is deferred to submit like an
  indirect one; at the split the build has run, and its instances are read
  there and then (`astrack::BringToBuild`). An older read never overwrites a
  newer one. This also ends the refusal of the FIRST dispatch on a
  GPU-written scene ("scene not read yet", 1 in the 0.49.0 Escher run): it
  is drawn now.
- **Measured:** the stale case and the first dispatch match, and so do, with
  GPU-written instances, the indirect and upload-indirect forms, the moved
  scene, decoy, bindless, mixed geometry and churn (12 layouts in one list,
  each deferred dispatch needing its own build). New dispatch suite cases
  `stalegpu`, `churngpu`, `indirectgpuinst`: all 41 cases pass, the gates
  behave. The gitest matrix 46 of 46; the Phase 4 probe matches with
  `-gpuinst`, `-gpuinst -debug`, `-openlist`, `-gfxsplit`, `-batchsplit`.
- **Cost.** Per top-level build with GPU-written instances: the save pass, a
  copy of 64 bytes per instance to a new readback buffer. A DIRECT lowered
  dispatch recorded after such a build, before its read arrives, costs a
  split and one CPU wait at submit (shared by consecutive dispatches), where
  it used to draw from the older read. Escher's lowered dispatches were all
  indirect, which already wait.
- **Refused by name:** a dispatch whose scene is not resolved while a live
  scene is not read from its latest build (0 unresolved in Escher).
- **Left unused:** `groupcount::RecordRawCopy` and `instance_copy.hlsl`, the
  old every-8-builds copy.
- A log line claimed an instance buffer outside the tracked resources made
  "the table assume zero". Untrue since 0.39.2 (the copy is by address);
  reworded.

## 0.51.0

**FIXED: a `GeometryIndex()` table built from an OLDER build of the scene was
drawn WRONG, silently.** Found while building the next item a gap, "the first
dispatch before a GPU-built scene's copy exists", which turned out larger than
listed.

- **The stale read.** GPU-written instances (Unreal's kind) are read back
  only every 8 builds of a structure, and always a submission late. The
  table copy wrote each record's geometry index from that read with no check
  that it came from the structure's LATEST build. `gitest.exe --stale` builds
  the scene with every instance's structure swapped, submits, then builds it
  again in place with the real ones: on 0.50.0, **5 of 7 layouts drawn wrong
  with nothing logged** (mult, slots, shared, anyhit, mixrs), and zero1 not
  drawn.
- **The missing copy.** The variant (the shim's own record layout) traces a
  copy of the scene, made only at a top-level build while switched on, or
  from a CPU snapshot. A pipeline created after a scene whose instances the
  GPU wrote got no copy: refused until the next build, and for a scene built
  once, never drawn. `gitest.exe --gpuinst` on 0.50.0: zero1 not drawn.
- **The fix.** The tracker now records which build each read came from
  (`astrack`, `*stale` from `GeometryLabels`). A scene not read yet, or read
  from an older build, goes to the variant, which needs no CPU read: its
  table is filled on the GPU from the copy's own contributions. And every
  top-level build with GPU-written instances gets them SAVED, copied verbatim
  into a buffer of the shim's by one small pass (`shim_scene.hlsl`'s new
  `verbatim` mode), so a dispatch can build the copy of exactly that build
  in its own list (`shimscene::Ensure`). That also serves a pipeline tracing
  with more argument pairs than the copy was built for, refused until now.
- **A copy built at a dispatch belongs to that list.** The one built from a
  CPU snapshot used to be registered for every list, so another list
  submitted first could trace it before it existed. Copies are kept per
  list now and dropped at its Reset.
- **Measured:** `--gpuinst` and `--stale`, 7 of 7 at 6.5 and 6.6, and each
  layout alone in its own process (28 of 28), where the log shows the copy
  built from the saved instances. The gitest matrix, 46 configurations,
  all match; the dispatch suite passes; the Phase 4 probe matches with
  `-gpuinst`, `-gpuinst -debug` and `-openlist`.
- **Cost:** one pass copying 64 bytes per instance after each top-level build
  whose instances are in GPU memory, for every application, whether or not a
  `GeometryIndex()` pipeline exists. Cheaper than being late: gating it on
  such a pipeline existing would leave a scene built before the pipeline
  undrawable.
- **The same stale read feeds the RayQuery path's table** (record kinds and
  the geometry index and contribution pairs, `rq_pipeline.cpp`), with no
  check either. Read from the code, not yet measured; not changed here.

## 0.50.0

**FIXED: an indirect DispatchRays of a `GeometryIndex()` pipeline was drawn
WRONG, silently.** The brief listed indirect DispatchRays as "not built,
refused by name". It was not refused. Both indirect paths bypassed the
shim's `DispatchRays`: with CPU-visible arguments the dispatch was forwarded
straight to the real list, and with GPU-written ones the split issued a plain
`DispatchRays` at submit. Either way the hit shaders ran against the
APPLICATION's table, without the geometry index the shim writes into its
copy, and nothing was logged.

- The `GeometryIndex()` dispatch is now one function taking the list, the
  bindings, the resolved scene and how to restore them, used by
  `DispatchRays`, by the CPU-visible indirect path (which now calls the
  shim's own `DispatchRays` for every pipeline) and by the split at submit,
  whose scene is resolved when the dispatch is recorded.
- **Measured:** `gitest.exe --indirect` (arguments in upload memory) and
  `--indirectgpu` (in GPU memory, so the split): 7 of 7 layouts bit-exact in
  all three construction modes at 6.5 and 6.6, and with the bindless scene.
  The whole matrix, 33 configurations, all match. With the old paths put
  back, both modes diverge in 7 of 7 layouts. Dispatch suite all pass; the
  Phase 4 probe through the proxy still matches WARP on indirect
  DispatchRays with both argument shapes.
- The lesson, same as ever: **a gap listed as refused has to be shown
  refused.** This one was never checked, and it drew wrong.

**Also: subobjects a library declares itself** (HLSL `LocalRootSignature`,
`SubobjectToExportsAssociation`, hit groups, configs), the last "associations
from inside a library" gap, checked the same way first: with them,
`CreateStateObject` FAILED on the 1070 (loud, not wrong; the debug layer:
`ClosestHitOther` "not fully bound"). Two causes:

- **A disassembly carries a library's subobjects only as comments**, so every
  library the shim rewrites lost all of them. Now each is declared again at
  state object scope (`CarryLibrarySubobjects`), read from the RDAT subobject
  table (part 6; layouts of every kind read off a DXC 1.10 compile), a root
  signature's bare RTS0 part decoded and serialized again by D3D12. Its
  associations follow the spec's "Subobject association behavior", read from
  the spec itself: anything the state object associates, explicit or
  default, overrides a directly included library's association, so none is
  carried there; a default declared in a library reaches that library's
  exports only, so it is spelled out as them; an associable subobject is
  declared only when an association needs it, since an unassociated one at
  state object scope would become a default that reaches everything.
- **The extended local root signature of a hit group whose signature comes
  from a library** is now built from that signature, in the spec's order:
  the state object's explicit association, its default, the library's
  explicit association, the library's default.
- **Measured:** `gitest.exe --libassoc` (both local root signatures, their
  associations and all the rest declared in HLSL): 7 of 7 bit-exact at 6.5
  and 6.6, also with the indirect split. The matrix, 34 configurations, all
  match.
- **Refused by name:** a rewritten library with its own subobjects included
  through an EXPORT LIST (the spec does not say which subobjects an export
  list includes; this is `gitest --libassoc --collections`), and a library
  association to a subobject declared in another library.
- **Known noise, predates this:** building the variant asks D3D12 for the
  identifier of every export, hit shaders included, and the debug layer
  warns for each ("not a shader type that supports producing shader
  identifiers"). Harmless; the answer is skipped.

## 0.49.0

**The lowered RayQuery path judges a dispatch against the ONE scene it
traces.** Until now `rq_pipeline` built its table, its record constants and
its refusal from every live top-level structure together, so two live scenes
that disagree about a record refused the dispatch: the 0.40.0 Escher runs'
"two live structures disagree", which drew nothing for that pass. It now uses
the resolution 0.47.0 and 0.48.0 built for `GeometryIndex()`.

- **At creation** the lowered library's scenes are read off its text
  (`gidx::ScanScenes`, the same scan, now shared: the scene fields moved into
  `gidx::Scenes`). Checked on the 64 lowered Unreal libraries from the Escher
  dumps: every one traces `ResourceDescriptorHeap[i]` with `i` read from the
  cbuffer at b0 space0, the shape the scan recognises.
- **At `Dispatch`** the scene is resolved through the bound compute root
  signature, and `TableWouldBeWrong`, `RecordKinds` and `RecordConstantsTable`
  take that scene alone (`only`). An indirect dispatch is resolved when
  recorded and judged at submit. Not resolved: every live scene, as before.
- **A resolved scene whose instances are not read yet is REFUSED**, as the
  `GeometryIndex()` path already does, rather than drawn from other scenes'
  data. Transient: GPU-written instance descriptions arrive a submission late.
- **Counted**, in the `stats:` line: scene resolved, not resolved, and
  refused because the scene is not read yet. The next Escher run says whether
  Unreal's b0 really is a CPU-readable root CBV, which is INFERRED from its
  source (`LooseParameterCBVIndex = 0`), not measured in the game.
- **Measured:** two new dispatch cases, `decoy` (a second live structure with
  a conflicting layout, scene through the root SRV) and `bindlessrq` (the
  scene from the heap, index in the root CBV, the decoy in the neighbouring
  slots and at t0): both bit-exact against WARP. With resolution switched off
  both are refused, 13872 hits missing; with the heap slot read one off,
  `bindlessrq` diverges by 4624. Dispatch suite all pass, 38 cases.
  `raytest` takes `--decoy` and `--bindless`.

## 0.48.0

Tier 1.1 item a, part 3 of the remaining list: **a scene reached through the
descriptor heap, Unreal's bindless form.** Read off the Escher dumps first:
all 64 of Unreal's scene handles are `ResourceDescriptorHeap[i]`, with `i` a
dword of the cbuffer at b0 space0 at a constant row (seven different
offsets), and Unreal binds b0 as a root CBV in upload memory
(`D3D12RayTracing.cpp`, `LooseParameterCBVIndex = 0`).

- **The scan** follows a TraceRay's handle through `createHandleFromHeap` to
  `extractvalue` of `cbufferLoadLegacy` at a constant row, and back to the
  cbuffer's resource record: (space, register, byte offset). Shape read off
  DXC at lib_6_6.
- **At the dispatch** the index is read from the bound root constants, or
  from the root CBV's memory when the CPU can read it (upload or readback
  heap, at record time, as the shim already reads upload-heap instance
  descriptions), and the heap slot is looked up in the bound shader-visible
  heap (`scenebind::LookupSlot`).
- **The root SRV rule is gone.** Since 0.44.0 a bound root SRV that was a
  known scene counted as THE scene whenever resolution failed. That is wrong
  when the shader traces another scene, a heap-indexed one say, and the
  application binds a different structure as a root SRV beside it. An
  unresolved scene is now judged over every live scene, which can refuse but
  never draws wrong.
- **Measured:** `gitest.exe --bindless` (index in a root CBV in upload
  memory, Unreal's way) and `--bindlessrc` (index in root constants), with
  the conflicting decoy scene in the neighbouring heap slots AND bound as a
  root SRV: 7 of 7 layouts bit-exact in all three construction modes, and the
  root SRV, table and 6.5 runs unchanged: 126 of 126. Reading the slot one
  off draws the decoy and diverges in all 7, both modes. Six cold-cache
  trials clean. Dispatch suite all pass.
- **Open, not explained:** one `--bindlessrc` run reported FAILED, the first
  run of that mode after the build, with its output cut off, so which layout
  and why are not known. 56 runs since, 6 of them with NVIDIA's shader cache
  cleared first, are all clean.
- **Still not resolved, and then judged over every live scene:** the index in
  a cbuffer in GPU-only memory, in a descriptor table CBV or a local root
  signature, or computed by arithmetic rather than read.
- **Not yet used by the RayQuery path:** the 64 dumped Unreal compute shaders
  lower through `rq_pipeline`, which still judges their layout over every live
  scene (the 0.40.0 "two live structures disagree" refusals).

## 0.47.0

Tier 1.1 item a, part 2 of the remaining list: **a scene bound through a
DESCRIPTOR TABLE, with several scenes live.** Until now a dispatch knew its
scene only from a root SRV; otherwise it judged the layout over every live
scene, which refused layouts that were fine (NOT DRAWN) and never found the
scene copy the shim's own layout needs. Now the shim resolves exactly which
scene each dispatch traces:

- **Which register each TraceRay traces** (`SceneRegs` in
  `proxy/geom_index_so.cpp`): its handle followed back through
  `annotateHandle` and `createHandleForLib` to the load of a resource global,
  through a getelementptr for an array element (constant or dynamic), and
  that global's `!dx.resources` record. Shapes read off DXC at lib_6_5 and
  lib_6_6.
- **Which descriptors hold a scene** (`proxy/scene_bind`): every SRV of
  RAYTRACING_ACCELERATION_STRUCTURE written through the wrapped device is
  recorded, any other view written over it forgets it, CopyDescriptors and
  CopyDescriptorsSimple carry it along, a new heap forgets its slots.
- **At the dispatch** (`gidx::ResolveScenes`): the register through the bound
  global root signature, a root SRV or a descriptor table range (explicit
  offsets and APPEND), to the descriptor, to the structure. When every
  register resolves, only those scenes count, and the variant takes that
  scene's copy.
- **Found on the 1070, not on WARP: CPU descriptor handles are not
  addresses.** NVIDIA's are small encoded numbers, interleaved between heaps
  (one heap's slots 0x1, 0x21, 0x41, another's 0x2, 0x22), so a byte range of
  one heap contains another heap's descriptors. The first version erased by
  range and lost them. Everything now works on exact handles, start plus slot
  times the increment.
- **Measured:** `gitest.exe --table` binds the scene through a table whose t0
  is its third descriptor, with a second scene live whose layout conflicts,
  its descriptor on both sides of the real one, and the real descriptor
  copied from a staging heap over a decoy. All 7 layouts bit-exact against
  WARP in all three construction modes at 6.5 and 6.6, 42 of 42, and the root
  SRV runs still 42 of 42. Resolving one slot off draws the decoy scene's
  geometry indices and diverges in all 7. Dispatch suite all pass.
- **Costs:** a hash lookup per CBV, SRV or UAV descriptor written, and per
  descriptor copied, only once any scene descriptor has been seen.
- **Still not resolved, and then the old rule applies (bound root SRVs, else
  every live scene; refuses, never draws wrong):** a scene reached through a
  descriptor HEAP index (SM 6.6 bindless, which Unreal's bindless ray tracing
  uses), through a local root signature, or through an unbounded array.
  Descriptors written through a device the shim did not hand out are not
  followed. And the shim's own layout still refuses a pipeline whose TraceRay
  calls trace different scenes, since it has one scene copy.

## 0.46.0

Tier 1.1 item a, third part: **`GeometryIndex()` when one hit group record is
reached by SEVERAL geometries**, a TraceRay multiplier of 0 for instance.
No record can say which geometry was hit, so the shim gives the dispatch a
record layout of its own, where every (instance, geometry, TraceRay argument
pair) has a record: `q + k * geometry + instance * block`. The hardware
computes that only from the TraceRay arguments and the instances'
contributions, so:

- **A variant pipeline.** Every TraceRay in it traces the shim's copy of the
  scene with (q, k), q the index of the call's original (R, M) pair and k the
  number of pairs (`phase5/rewriter/shimtrace.py`, C++ port in
  `proxy/rewriter/geom_index.cpp`, byte-identical; shapes read off DXC in
  `phase5/cases/reference/lib_shimtlas_ref.hlsl`). The copy is bound through a
  root descriptor at `t0, space 0x7FFF0000` appended to each raygen's local
  root signature. The association logic is now one helper (`Rebuild`), used
  for the geometry index constant and for this descriptor alike. Collections
  are handled: a collection that traces is kept, and a pipeline's variant
  links a variant of it; the rest are linked as they are. Built at pipeline
  creation when a multiplier is 0, otherwise at the first dispatch that needs
  it.
- **The shim's copy of the scene** (`proxy/shim_scene`). Once a variant
  exists, every application top-level build is followed, in the same list,
  by a small pass (`proxy/shim_scene.hlsl`) that copies the instance
  descriptions from the application's buffer with each contribution replaced
  by `instance * block`, and a build of the shim's own structure from them:
  so the copy is always exactly the scene just built. For a pipeline created
  after its scene was built, the copy is made from a snapshot of that build's
  instance descriptions when they were CPU-visible
  (`astrack::InstanceSnapshot`, dropped at every rebuild).
- **The variant's tables** (`proxy/shim_table.hlsl`): the hit group table in
  the shim's layout, each record filled from the application's record for
  that hit (`R[q] + M[q] * g + contribution`) with its geometry index
  written in; raygen, miss and callable tables copied with every identifier
  the variant changed swapped, and the scene copy's address written into the
  raygen record. The dispatch binds the variant, dispatches, and binds the
  application's pipeline again.
- **Measured:** `gitest.exe`, all 7 layouts bit-exact against WARP as one
  state object, as collections, and as collections grown by AddToStateObject,
  at lib_6_5 and lib_6_6: 42 of 42. With `DXR_TIER11_GI_POISON=1` the two
  shared-record layouts diverge in all three modes. Dispatch suite 36 of 36;
  rewriter checks pass with the new byte-identity check.
- **Costs, stated:** once a variant exists, one extra top-level build and one
  small pass per application top-level build, a second compiled pipeline, and
  per dispatch four small table passes. Unreal's layout never needs it.
- **Still not built, refused by name:** a scene bound through a descriptor
  table (item 2), and in the shim's layout also a closest-hit or miss that
  calls TraceRay (recursion above 1), more than 15 TraceRay argument pairs,
  and TraceRay arguments computed at run time. A variant first needed at a
  dispatch whose scene was built on the GPU before the variant existed is NOT
  DRAWN until that scene's next build, which engines do every frame.

## 0.45.0

Tier 1.1 item a, second part: **`GeometryIndex()` through COLLECTIONS, the way
Unreal builds its ray tracing pipelines.** Read from
`D3D12RayTracing.cpp`: every shader is compiled into its own
`D3D12_STATE_OBJECT_TYPE_COLLECTION`, exports RENAMED, one local root
signature associated to each shader export by name (never to the hit group),
and the pipeline is only `EXISTING_COLLECTION` subobjects, often grown by
`AddToStateObject`, with the shader identifiers taken from the collections.

- A collection whose library reads `GeometryIndex()` is transformed like a
  pipeline, and the extended hit groups are remembered on it. A pipeline
  linking it merges them under the names its `EXISTING_COLLECTION` gives them
  (a subset, renamed).
- A collection holding a shader that can call TraceRay records its TraceRay
  arguments for the pipelines that will link it. Which shaders can is read
  from the container's `RDAT` function table (layout checked on a DXC 1.10
  library: kind in word 4, 7 raygen, 10 closest-hit, 11 miss), so only a
  raygen, or with a recursion depth above 1 a closest-hit or miss, is ever
  disassembled. Unreal's depth is 1: only its raygen collections pay.
- The AddToStateObject emulation's rebuild goes through the same path, so a
  pipeline grown by collections is served.
- **Measured:** `gitest.exe --collections` (raygen and hit collections,
  renamed exports, per-shader associations) and `--grow` (the hit collection
  linked by AddToStateObject) each give the same 5 of 7 layouts bit-exact
  against WARP as one state object does, at lib_6_5 and lib_6_6; the poison
  check diverges in both. Dispatch suite 36 of 36.
- **Still not built, refused by name:** records several geometries reach, a
  top-level structure bound through a descriptor table while several are
  live, TraceRay arguments computed at run time, indirect DispatchRays of such
  a pipeline, the first dispatch before instance data is read, associations
  from inside a library. Escher's debug view has not been run.

## 0.44.0

Tier 1.1 completion, item a, first part: **`GeometryIndex()` in an
application's OWN DXR 1.0 hit shaders.** Tier 1.1: the GTX 1070's driver
rejected the whole `CreateStateObject`. In Unreal it is one shader, the ray
tracing debug view's closest-hit (`RayTracingDebugMainCHS`), and a failed
pipeline there is fatal once used, so opening that view was inferred to crash.

- **Found on the way:** `GeometryIndex()` sets SFI0 bit 20, the SAME bit as
  RayQuery, so the shim logged such a library as "RayQuery". The message now
  names both.
- **How:** a DXR 1.0 hit shader can only tell geometries apart by WHICH RECORD
  it runs from. So each `GeometryIndex()` read becomes one 32-bit constant at
  `b0, space 0x7FFF0000` (`phase5/rewriter/geomidx.py`, C++ port
  `proxy/rewriter/geom_index.cpp`, byte-identical, shapes read off DXC in
  `phase5/cases/reference/lib_shimgeom_ref.hlsl`). At `CreateStateObject` the
  shim appends that constant to the local root signature of every hit group
  whose shaders read it, re-associated to exactly those exports, so the
  application's own local arguments keep their offsets (`proxy/geom_index_so`).
  At `DispatchRays` a small compute pass (`proxy/geom_table.hlsl`) copies the
  application's hit group table into the shim's own buffer and writes each
  record's geometry index into it, worked out from the instances and the
  TraceRay arguments read from the libraries. The application's table is
  never written. Root signature blobs are now kept on every root signature
  object, so a local one can be extended.
- **When the dispatch binds its top-level structure as a root SRV, only that
  structure counts**; otherwise every live one does, which can only find more
  collisions.
- **Measured:** `tier11\gitest.exe` (`build_tier11.bat`) is a small DXR 1.0
  application with seven shader table layouts, run on WARP and through the
  shim on the 1070. Five match WARP bit for bit at lib_6_5 and lib_6_6:
  one record per geometry (Unreal's layout), two interleaved ray types, two
  instances sharing a structure, an any-hit that reads it and changes which
  surface is hit, and two local root signature layouts in one table. The
  application's own record data arrives intact in all. With
  `DXR_TIER11_GI_POISON=1` all five DIVERGE. Dispatch suite 36 of 36,
  rewriter checks 45 of 45 plus the new byte-identity check.
- **Not built yet, each logged as NOT DRAWN or REFUSED with the reason, never
  drawn wrong:** a layout where one record is reached by several geometries
  (the two remaining test layouts, a TraceRay multiplier of 0), hit groups in
  an `EXISTING_COLLECTION` (how Unreal builds pipelines, so Escher's debug
  view still does not work), a top-level structure bound through a descriptor
  table while several are live, TraceRay arguments computed at run time, an
  indirect DispatchRays of such a pipeline, the first dispatch before the
  scene's instance data has been read, and local root signatures associated
  from inside a library.

## 0.43.0

- **Several RayQuery objects in one entry point lower.** 33 of Escher's
  shaders got do-nothing pipelines for "2 concurrent RayQuery objects":
  `LumenScreenProbeGatherHardwareRayTracingCS` (16),
  `LumenRadiosityHardwareRayTracingCS` (8),
  `LumenSceneDirectLightingHardwareRayTracingCS` (8) and
  `NiagaraCollisionRayTraceCS` (1). The check counted allocations. Profiled
  from the dump: every one is two queries one after the other, each with its
  own Proceed loop and one triangle commit.
- **How:** each query becomes its own `TraceRay`, with its own payload and
  raygen names (`%rq.q1.pl`, `%rq.q1.flags`; the first query keeps `%rq.`, so
  one-query output is byte-identical to before). ONE generated any-hit holds
  every loop body and branches on a query id the raygen stores in payload
  field 11 before each trace; carried values move to field 12 on. The shader
  table does not change: one hit group serves every trace. Record data, the
  closest-hit's matrix fetch and the declarations are the union over queries,
  and the shim builds the any-hit whenever ANY query needs one.
- **Refused by name:** a query traced inside another's Proceed loop (that loop
  becomes an any-hit, which cannot call `TraceRay`), and a multi-query shader
  in which a query commits procedural hits (an intersection shader has no
  payload to read the query id from). The multi-query checks run before the
  per-query ones, so a nested query is refused for being nested.
- Measured: every shader in the 0.40.3 and 0.41.1 dumps now lowers, 246 of
  246, Python and C++ byte-identical, all validate and sign. The 33 build 99 of
  99 cold on the 1070 with the NVAPI slot registered. Case `twoq`
  (`rayquery_twoq_sm66.hlsl`): the two loop bodies commit opposite triangle
  halves and the second carries a prefilled UAV value; bit-exact against WARP
  on two seeds, and on WARP itself swapping the bodies changes 11536 rays, so
  a wrong dispatch cannot pass. Dispatch suite 36 of 36 plus gates, rewriter
  checks 45 of 45.
- Not hit yet, recorded: the CFG model does not follow `switch` edges, and 55
  of the 125 dumped shaders contain one.

## 0.42.0

- **The 24 MegaLights light-sampling shaders lower.** In the 0.41.1 run they
  got do-nothing pipelines, "Proceed loop body reads values defined outside
  it": `HardwareRayTraceLightSamplesCS` (16) and
  `VolumeHardwareRayTraceLightSamplesCS` (8). Each reads its light sample
  before the trace, `RWLightSamples[SampleCoord]` and
  `RWLightSampleRays[SampleCoord]` (UAVs) plus a structured and a typed buffer
  (SRVs), and uses what it read in the shadow test inside the loop.
- **A value read from a READ-ONLY resource is read again** in the hit shader:
  `textureLoad`, `bufferLoad` and `rawBufferLoad` on an SRV join the values the
  hit shader may rebuild. Exact, since a dispatch cannot write what it reads as
  an SRV, and an out-of-range load returns zero rather than trapping.
- **Any other value the hit shader cannot rebuild travels in the PAYLOAD.** The
  raygen stores it just before `TraceRay`, the any-hit loads it at entry under
  its original name, so the loop body is transplanted unchanged. Fields go
  after the fixed ones, so nothing else moves: the payload is 92 bytes plus 4
  per value, and the shim sets `MaxPayloadSizeInBytes` per pipeline. MegaLights
  needs one. Up to 16 `i32`, `float` or `i1` values; refused by name: more
  than that, a value computed after `TraceRayInline`, and a loop body that
  becomes an intersection shader, which has no payload.
- **A UAV is not re-read, and the first version did.** It re-read a UAV when
  nothing in the shader wrote before the trace. The old `refuse_uav_in_loop`
  test failed on it, rightly: that shader reads another thread's slot, so two
  re-reads during one ray can differ, which the original, reading once, never
  does. The test is flipped to check the value comes from the payload.
- Measured: Python and C++ byte-identical on all 246 shaders of the 0.40.3 and
  0.41.1 dumps, the loop-isolation refusal gone from both; the 24 validate and
  build 72 of 72 cold on the 1070 with the NVAPI slot registered. Case
  `preload` (`rayquery_preload_sm66.hlsl`, `raytest --prefill N`) reads its own
  prefilled output slot and a read-only buffer before the loop: bit-exact
  against WARP with two seeds, and seed 1 against seed 2 diverges on 14450
  rays, a gate in the suite. Dispatch suite 35 of 35 plus gates, rewriter
  checks 39 of 39.
- `sotest` reads `payload=N` from the shape line the shim now writes.
- Scope, set by the user: nothing is given up; see the brief.

## 0.41.1

- **0.41.0 crashed Escher 3 runs of 3, and the fix is to what 0.41.0 got
  wrong.** Each run lost the device, `DXGI_ERROR_DRIVER_INTERNAL_ERROR`,
  inside the shim's `CreateStateObject`, on exactly the libraries that carried
  the newly lowered "append". It was never an append. It is NVIDIA's HLSL
  extension encoding (`nvHLSLExtnsInternal.h`): a counter increment on a
  `RWStructuredBuffer<NvShaderExtnStruct>` at u0 space1001, the opcode stored
  at offset 0, `rq.RayFlags()` at offset 76 to name the query, and a second
  increment whose result is the answer. Op 94 is
  `NV_EXTN_OP_RT_GET_CANDIDATE_CLUSTER_ID`, in the Proceed loop; op 95, the
  committed form, sits after it. The "constant 94" and "RayFlags at offset 76"
  that 0.41.0 and the brief read as a debug record were the opcode and the
  query handle.
- **Why only in the game.** Unreal calls
  `NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread(u0, space1001)` around
  every compute pipeline it creates with a vendor extension
  (`WindowsD3D12PipelineState.cpp`), and the shim's `CreateStateObject` runs
  inside that call, on that thread. The driver then compiles a RayQuery
  intrinsic into a hit shader with no RayQuery, and dies. Every offline replay
  lacked the registration. `sotest` now has it, `SOTEST_NVEXT="0,1001"`:
  the two Escher libraries fail 10 of 10 with the game's exact error, and
  build 24 of 24 cold without it; a library with no extension buffer builds 5
  of 5 with it.
- **The fix, in both rewriters, byte-identical:** a pre-pass on the normalised
  text, `phase5/rewriter/nvapi.py` and `proxy/rewriter/nvapi_fold.cpp`, finds
  the extension buffer by its type, replaces each op 94 and 95 call with
  0xFFFFFFFF and removes its stores and counters, and refuses any other use of
  that buffer. 0xFFFFFFFF is "not a cluster", the value Unreal itself uses
  when cluster operations are off; measured on the 1070,
  `NvAPI_D3D12_GetRaytracingCaps(CLUSTER_OPERATIONS)` is 0x0, `CAP_NONE`, so
  no geometry can be a cluster. A module without the buffer passes through
  unchanged.
- Measured: the 125 shaders dumped from the 0.40.3 run, Python and C++
  identical on every one; 40 carry the extension buffer and 18 of those now
  lower (the rest hit the concurrent-query and loop-isolation refusals); every
  lowered output validates and none keeps a counter update. On the 1070, cold
  cache, slot registered: those 18 build 90 of 90; the same 18 lowered the
  0.41.0 way fail 5 of 5, and the Escher library 3 of 3, in the same harness.
- `phase5/cases/rayquery_nvapi_sm66.hlsl` writes both calls the NVAPI way.
  Three new checks in `test_reject.py`: the calls fold and nothing is left, an
  op other than 94 or 95 is refused, and a call without its result is refused,
  each also checked in the C++; with the fold off, all three fail. The
  rewriter suite gains a driver check: the folded library builds with the slot
  registered, and the same case lowered without the fold
  (`phase5/rewriter/nofold.py`) must kill the driver, which it does.
- **Probably also the 0.36 crash that never reproduced offline.** That was
  `RayTracingDebugMainCS`, 13132 bytes, which is one of the two Escher
  libraries here, and it carries the same calls. Not re-run: that dump is
  gone.
- Unchanged: a genuine append still lowers, and
  `NO_DUPLICATE_ANYHIT_INVOCATION` is still set on every bottom-level
  geometry. No Escher shader lowered so far appends for real.

## 0.41.0

- **A Proceed loop that APPENDS is lowered, not refused.** 47 of Escher's
  refusals, MegaLights light sampling and the Lumen translucency, direct
  lighting and debug passes, were one construct from Unreal's shared
  TraceRayInline wrapper: under a runtime flag, a counter increment and
  stores at the index it returned, a debug record per candidate. The rule,
  in both rewriters: a loop body may keep `bufferUpdateCounter` and
  `bufferStore`/`rawBufferStore` whose index is a counter result from the same
  body. Anything else that writes is still refused, and so is an append next
  to `Abort()` (the lowering lets traversal continue after it) or in a body
  that becomes an intersection shader (which the spec lets run more than once
  per primitive whatever the flags say).
- **Why it is the same append:** its result does not depend on the order of
  candidates, which is undefined for RayQuery and any-hit alike, only on how
  many there are. The spec lets an any-hit run more than once per
  intersection unless the geometry carries
  `D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION`, and lists
  no such exception for `Proceed()`; so the proxy sets that flag on every
  bottom-level geometry, in `BuildRaytracingAccelerationStructure` and in
  `GetRaytracingAccelerationStructurePrebuildInfo` alike, so the sizes the
  application allocated are the build's. Legal for the application's own
  any-hit shaders, possibly slower. Read from the spec, Raytracing.md, the
  geometry flags and "observable duplication"; that `Proceed()` yields each
  candidate once is the reading of that list, not a sentence.
- **A parser bug it uncovered, the next refusal behind this one:** a call with
  a metadata attachment, `, !dx.precise !20`, did not match the call pattern
  in either model, so a RayQuery accessor carrying one was never rewritten and
  kept naming the deleted query handle: "use of undefined value" from the
  assembler, 17 of the 18 dumped shaders. Probably the one assembler failure
  the brief listed as unexplained. A test in `test_reject.py` adds the
  attachment to `CommittedRayT` and fails against either old parser.
- Measured: all 18 side-effect refusals dumped from the 0.40.3 run now lower,
  validate and sign, byte-identical between the Python and the C++. On the
  1070, each built with a root signature borrowed from the same dump plus the
  bindings the debug layer named missing (the debug append buffer at u0,
  space1001): 54 of 54 cold compiles clean, where the vendored crash library
  crashed 7 of 8 under the same procedure.
- `raytest --append` binds a counter-backed UAV at u1 and writes the appended
  records, sorted, to `<out>.append`. Case `append`
  (`rayquery_append_sm66.hlsl`, `--multi`): 18496 records, sorted sets
  identical to WARP's, image bit-exact; a variant storing each record plus one
  differs, so the comparison can fail. Dispatch suite 34 of 34 plus 5 gates;
  rewriter suite 28 of 28 checks, each new verdict also checked in the C++.
  The Tier 1.1 probe through the proxy still matches WARP with the flag on.
- `sotest`: `DXR_TIER11_DEBUGLAYER=1` prints what the debug layer says about
  `CreateStateObject`, and `SOTEST_ADD_RANGES="u0:1001,b4:0"` appends a table
  of those ranges to a borrowed root signature. Test only.
- NOT verified: that the flag changes anything observable on this driver.
  With one candidate per ray no duplicate any-hit was ever seen, flag or not.


- **Spare shader tables are reused in the open world too.** 0.40.2 in Escher
  freed the leak (7105 tables built, 5859 freed), but reuse stopped at 1169
  once the open world loaded: the idle-spare trim kept the OLDEST four, which
  were the menu's smaller tables and never fit, and freed the newer ones that
  did. It now keeps the newest idle spares of the size just built and frees
  the rest. `raytest --churnflush` jumps to a bigger table halfway through to
  show it: at 40 layouts, 0.40.2 put 13 of 41 tables into a spare and freed
  17, 0.40.3 puts 24 and frees 9.
- **A structure the scene moved away from stops counting.** Unreal moves its
  top-level structure to a new buffer when it outgrows the old one; the old
  one stayed live for 64 builds, the two disagreed about records, and every
  lowered dispatch was refused meanwhile (59 and 178 in the last two Escher
  runs). Now a structure is SUPERSEDED once another one, first built after its
  last build and with read instance data, has been built 4 times without it.
  Two structures an engine keeps alive together are both rebuilt and neither
  supersedes the other. The cost: a structure built once and traced forever
  stops counting after 4 builds of a newer one, not 64.
- `raytest --move`, with `--geom --contrib`: dispatch, then build the scene
  at a new address with the contribution one higher, 4 times, and dispatch
  again. Case `move`: 0.40.2 refuses and diverges by 9248 hit/miss
  mismatches, 0.40.3 matches. Dispatch suite 33 of 33 plus 5 gates.


- **Old shader tables are freed.** A lowered pipeline builds a new table
  whenever the scene's record layout changes, which in Escher's open world is
  almost every frame, and kept every evicted one until the pipeline died:
  10137 tables in 7 minutes at 0.40.1, about 3 GB of upload heap by the end,
  by the code's arithmetic. New `proxy/gpu_hold.{h,cpp}` records which command
  list uses which table, stamps the use with a per-queue fence when the list
  is submitted, and ends it only when the list can never run again (Reset or
  destroyed) AND the fence has passed. An evicted table is reused for the next
  layout of the same size once idle, and idle spares beyond four are released.
  Table sizes are rounded up to a multiple of 4096 records so that nearby
  layouts share a size.
- `raytest --churn N`, with `--geom`: after the real dispatch, in the same
  unsubmitted list, N more layouts and dispatches into a decoy, so the real
  dispatch's table is evicted before anything has run. `--churnflush` submits
  after each layout so the reuse path runs. Cases `churn` and `churnflush`,
  both bit-exact against WARP; 13 of 21 tables went into reused buffers in
  `churnflush`. A new gate runs `churn` with `DXR_TIER11_NOHOLD=1`, a test-only
  switch that makes every table look idle, and requires DIVERGE: it gives
  13872 hit/miss mismatches, every hit.
- **A crash at process exit, found and fixed before it shipped.** The first
  build kept its per-queue fences in a static map, released when the DLL
  unloaded, by which time D3D12 may already have been unloaded: `raytest`
  exited with 0xC0000409 on 4 of 6 runs, which the suite reported as 15 to 19
  failed hardware runs, a different set each time. The state is now allocated
  once and never destroyed; 8 of 8 clean, suite 32 of 32. `as_tracker` keeps
  its own fences and readbacks the same way and has not been seen to crash,
  but it is the same pattern.
- Stats line: `tables built N (into a spare R), reused C, buffers freed F`.


- **Counters for lowered dispatches.** One `stats:` line every 10 seconds
  while a count moves, and one `stats at exit:` line: dispatches DRAWN (and
  how many indirect), REFUSED by reason, tables built and reused, top-level
  reads new, changed and unchanged, and the live top-level structures with
  how many builds ago each was rebuilt. Each distinct pair of live structures
  that disagree about a record gets one `conflict:` line naming both.
- Why: the 0.40.0 Escher run refused a lowered dispatch in the open world for
  two live top-level structures (271 and 270 instances) disagreeing about
  record 2068, and could not say how often. The refusal logs once, the table
  line stops at 32 and the re-read line at 8, and all three were spent before
  the open world loaded. A counter cannot be spent.
- No behaviour change: dispatch suite 30 of 30 plus gates. The `conflict:`
  line is not exercised offline; no harness scene has two live structures
  that disagree.

---

## 0.40.0

- **THE DRIVER CRASH'S REAL CAUSE: AN UNANNOTATED HANDLE, NOT A CBUFFER.**
  Since 0.36.x this project has held that a hit shader READING a cbuffer
  through the local root signature crashes the Pascal driver inside
  `CreateStateObject`, and 0.37.0 to 0.39.x baked the record values into
  copies of the hit shaders to avoid the read. It was the handle. At Shader
  Model 6.6 DXC follows every `createHandleForLib` with an `annotateHandle`;
  the lowering did not, for the record resource it adds. Measured one
  variable at a time on the vendored Unreal shader, cache cleared every
  trial: cbuffer record unannotated 9 of 15, annotated 0 of 15; raw buffer
  record unannotated 10 of 15, annotated 0 of 15. The local binding is part
  of the condition: the old `globalrec` control kept the bare handle, bound it
  globally, and was clean.
- **How the wrong answer was reached.** Every earlier variant changed how the
  record was delivered and none changed how its handle was formed. The first
  local root SRV variant was then copied from DXC's 6.6 output, which
  annotates, and read as "a root SRV is safe" at 0 of 30, when it had changed
  two things. The rewriter's own unannotated SRV output crashing 7 of 15 is
  what exposed it. See `phase5/cases/driver-crash/README.md`, corrected.
- **So the bake is gone and each record carries its pair again.** A hit group
  record is now its identifier plus an 8-byte GPU address, its local root
  signature ROOT SRV at t0, space1, pointing at its own (geometry,
  contribution) pair in the same buffer. The hit shader reads it with
  `rawBufferLoad` through an annotated handle at 6.6, and through DXC's 6.5
  form, which has no `annotateHandle`, at 6.5. No hit shader copies, no rebake
  at dispatch, no 256-pair cap: Escher's open world, 383 pairs, was skipped
  entirely at 0.39.x. The records' pairs, conflicts and live-structure rules
  are unchanged.
- Measured: the vendored crash shader through the 0.40.0 rewriter, 0 of 15
  cold; all 60 record-reading Unreal libraries from the last Escher run,
  lowered fresh, one cold compile each, 0 of 60. Dispatch suite 30 of 30; the
  record cases pointed at pair 0 on purpose diverge by the same 4624, 4624
  and 7396 as the bake's check did. Rewriter suite byte-identical between the
  Python and the C++, and the 6.6 record read is checked to be annotated.
- **A harness gap closed: nothing here had ever run a 6.6 shader through the
  proxy.** `raytest` compiled every `--cs` shader at `cs_6_5`, so the dispatch
  suite's `sm66` case tested 6.5 under a 6.6 name. A file named `*sm66*` is
  now compiled at `cs_6_6`, and `geomsm66` is the record case at 6.6.
- Removed: `phase5/rewriter/bake.py`, `proxy/rewriter/rq_bake.{h,cpp}`, the
  `bake` subcommands and the rebake path. `sotest` reads `recordsrv=1` from a
  shape file and builds the local root SRV itself.


- **0.39.1 got through the open world; the GPU hang is gone.** Escher, three
  runs: the open world loaded and ran, which it never did at 0.39.0, so the
  out-of-range table was the hang. The log shows the re-reads and the skip:
  the open world's cutscene scene needs 383 to 392 baked pairs.
- **Then the cutscene crashed, 3 of 3, on Unreal's command list `Close`
  returning `E_INVALIDARG`** (D3D12CommandList.cpp:244, RHI thread), 68 to
  110 seconds in, not a device removal.
- **Likely cause, INFERRED: the instance data copy.** 0.39.1 made it
  repeat, every 8 builds instead of once, and it worked by looking Unreal's
  instance buffer up BY ADDRESS in the resource tracker, which holds no
  references, then recording a barrier and a copy on whatever it found.
  During streaming that can be a resource Unreal already freed, and a list
  referencing a deleted resource fails `Close` with exactly this error: the
  0.38.0 crash was the same symptom from a freed buffer of the shim's own.
- **Now the copy never names Unreal's resource.** A ROOT SRV takes a bare
  GPU address, so a small shader of the shim's, `proxy/instance_copy.hlsl`,
  reads the instance descriptions from the very address the build was given,
  into buffers the shim owns. Nothing is looked up and nothing is
  transitioned: DXR requires the instance buffer in
  `NON_PIXEL_SHADER_RESOURCE` at the build, which is the state a shader read
  needs. The same principle as the group count capture in 0.39.0. The
  compute bindings are restored afterwards, the pipeline the application
  bound LAST last, since a state object and a pipeline state replace each
  other.
- The resource tracker records each buffer's heap type at creation, so
  choosing the upload-heap path no longer calls into a resource that may
  have been freed.
- **Fixed: shader tables rebuilt on every frame.** One of Escher's
  structures alternates between two layouts every frame (max contribution 14
  and 30), and 0.39.1 built a new table on each flip and freed none: 1095 in
  one run. Tables are now cached per layout, eight per pipeline, and a rebake
  retires the cache.
- Verified: dispatch suite 29 of 29; the Phase 4 probe with GPU-only
  instances reads the right answer through the new copy (2 instances,
  contribution 1, both kinds), clean under the debug layer apart from the
  probe's own creation warnings.


- **The first game run with lowered shaders dispatching hung the GPU in the
  open world, every time.** Escher at 0.39.0, `rqphase = 4`: the main menu ran;
  loading the open world gave `DXGI_ERROR_DEVICE_HUNG` (0x887A0006) 41 to 48
  seconds in, four runs of four. Two reports read "Shader compilation failures
  are Fatal", but that came after: the device was gone, the next
  `CreateStateObject` failed with 0x887A0005, and that shader was forwarded.
- **Verified from the log: the dispatches used a table far too small.** The
  only scene data the shim ever read was the menu's, 6 instances reaching
  record 26 or 10, so the open world's lowered dispatches used 27 and 19
  record tables. An earlier run read the open world: 270 instances,
  contribution up to 1998. An index past the table is undefined in DXR.
  INFERRED, not proven: that this is what hung the GPU. The alternative is a
  dispatch simply taking over two seconds in a dense forest on Pascal.
- **Fixed four ways.**
  - Instance data is re-read when a structure may have changed. It used to be
    read once per destination address, so a structure rebuilt in place when a
    level loads kept its first answer forever. CPU-visible descriptions are now
    read on every build (free); GPU ones when the instance count changes, every
    8 builds of that structure, and never while a copy of it is in flight.
  - Only LIVE structures count, ones rebuilt within the last 64 top-level
    builds, so the menu's stops counting. Two live structures disagreeing
    about a record is refused as a conflict, where it used to be merged
    first-read-wins, which would have baked one of them wrongly and silently.
  - The hit group table is PADDED to 4096 records or twice what is known. A
    padded record produces no hit: missing for a frame, never undefined. A
    triangle-only shader with no record data pads with its own record, which
    is still the right answer. With baked record data, a record no known
    instance reaches gets the no-hit record rather than copy 0's answer.
  - A scene needing more than 256 (geometry, contribution) pairs SKIPS the
    dispatch, said once, rather than baking hundreds of hit shader copies at
    about 34 ms each. **This means most lowered passes draw nothing in the
    open world**: 59 of the 63 shaders that lowered bake record data, and the
    open world has about 2000 records. Bringing them back needs the
    contribution read another way, see the brief.
- **Fixed: a list created open had no allocator recorded**, only `Reset`
  recorded it, so splitting such a list failed and 0.39.0 skipped some of
  Unreal's indirect dispatches ("split: no device or allocator").
- **New case `rebuild`**: `raytest --multi --contrib --rebuild` dispatches
  against a structure with every contribution 0, rebuilds it IN PLACE with 0
  and 1, and dispatches again. 0.39.0 gives 9248 of 18496 hits. 0.39.1
  matches, and the log shows the re-read. With the re-read poisoned it still
  matches, through the padding alone. Dispatch suite 29 of 29.


- **Indirect compute dispatch of a lowered RayQuery pipeline is emulated.**
  Unreal issues most of its Lumen and MegaLights inline passes with
  `DispatchIndirect`, read from the engine source: `ExecuteIndirect` with a
  signature of one `DISPATCH` argument, stride 12, no root signature, count 1,
  no count buffer, into a sub-allocated argument buffer. Until now the shim
  forwarded it, the GPU ran the do-nothing carrier, and the pass drew nothing
  with nothing in the log. Measured: the 0.38.1 DLL draws 0 hits where WARP
  draws 8117.
- **The group counts are read without touching the application's argument
  buffer.** Its state is whatever the application's barriers left it in, often
  `INDIRECT_ARGUMENT` combined with other read states, and a transition with a
  guessed `StateBefore` corrupts the application's own tracking. So the shim
  replays the application's own `ExecuteIndirect`, same signature, buffer and
  offset, with a tiny compute shader of its own bound, `proxy/group_count.hlsl`,
  where each group records `SV_GroupID + 1` with an atomic max. The buffer is
  already in the state `ExecuteIndirect` needs, because the application was
  about to make exactly that call. Everything the capture writes is the
  shim's. The compute bindings and the pipeline state are then restored.
- The rest is the indirect `DispatchRays` machinery: the recording is split,
  the counts are read after the segment runs, and `DispatchRays` with groups
  times numthreads goes out on a pooled list at submit time. A rebake for new
  record pairs can happen there too, inside the queue hook.
- Command signatures holding a `DISPATCH` argument are tagged at creation with
  private data, so a reused pointer never carries a stale answer. A count
  buffer, or a signature with other arguments or a root signature, is logged
  as `NOT EMULATED` and nothing is drawn for it.
- **A pipeline state given to `Reset`, `ClearState` or `CreateCommandList` is
  now tracked**, so a split's continuation and the capture can restore it.
  Before, only `SetPipelineState` was, and a continuation after a split lost a
  pipeline bound through `Reset`.
- **`rqphase = 4`** is the full path, dispatch included, with refused shaders
  still given a do-nothing pipeline, so the shaders that lower can run in an
  Unreal game while the refusal list is still long. It worked before by
  accident of the numbering; it is now documented and says so in the log.
- **Three new dispatch cases, 28 of 28 bit-exact.** `raytest --indirect` puts
  the arguments in a GPU-written buffer, in a COMBINED read state, at byte 36
  among decoy counts of 3; `--indirectup` in an upload buffer, read at record
  time; `indirectgeom` rebakes at submit. Sensitivity, both measured: the old
  DLL gives 0 hits against 8117; capturing at offset 0 reads the decoy, 3x3x3
  groups, and diverges. The debug layer reports nothing from the shim.


- **Fixed: a command list could fail to `Close` with `E_INVALIDARG`, which
  Unreal makes fatal.** One Escher run in five died on "hr failed at
  D3D12CommandList.cpp:244 with error E_INVALIDARG", which is
  `FD3D12CommandList::Close`, on the RHI thread, 18 seconds in.
- The cause was this shim. When a top-level build reads GPU-written instance
  data, the shim records a copy into a readback buffer it owns, then reads the
  copy once a fence it signals after a submission has passed, and drops the
  buffer. It stamped EVERY pending copy on EVERY submission, on the stated
  assumption that anything recorded had gone out by then. With several
  threads recording, that is false: another thread's submission passed the
  fence, the shim parsed a buffer the GPU had not written yet, and released
  a resource that a still-open list referenced.
- It also parsed garbage. Reproduced offline, the old DLL read "max
  contribution 0, geometry reached: none" for a scene whose truth is
  contribution 1 with both kinds, then the device was removed.
- Now a copy is stamped only by the submission that carries the list that
  recorded it, a list that is reset or destroyed unsubmitted drops its copies,
  and each queue has its own fence, because one fence signalled on several
  queues is not ordered.
- Almost certainly also the `#921 resource deleted prior to closing the
  command list` and `DEVICE_HUNG` of the 0.36.6 run, which named a resource
  in the middle of Unreal's acceleration structure builds. Inferred, not
  re-run.
- **New test: `tier11probe hw -openlist`.** It keeps the list holding the
  top-level build open while two other submissions run to completion, then
  closes it. The 0.38.0 DLL is killed by it (0xC0000409; under `-debug` the
  device is removed), 0.38.1 passes, clean under the debug layer, and reads
  the right instance data. Implies `-gpuinst`.
- The DLL deployed as 0.38.0 logged itself as 0.37.0: it was built before the
  version number was bumped. Its code was 0.38.0. Deployment now checks the
  version resource of the copied files, not only that their hashes match the
  build.

## 0.38.0

- **`SV_GroupID`, `SV_GroupThreadID` and `SV_GroupIndex` now lower.** 24 of
  the 129 refusals from the 0.37.0 Escher run were the validator rejecting
  `dx.op.groupId` (94) and `dx.op.flattenedThreadIdInGroup` (96) in the
  raygen, where no compute thread-index op is legal. Read from the engine
  source first: `LumenRadianceCacheHardwareRayTracingCS` takes `SV_GroupID`
  and `SV_GroupIndex`, unwraps them into a linear trace index, and has no
  groupshared memory or barrier in that entry point.
- The shim launches exactly groups times numthreads rays, one per thread, so
  `DispatchRaysIndex` IS each thread's `SV_DispatchThreadID`, and the group
  values follow exactly: `SV_GroupID.c = DRI.c / numthreads.c`,
  `SV_GroupThreadID.c = DRI.c % numthreads.c`, and `SV_GroupIndex` is those
  flattened, `(z * ny + y) * nx + x`. Emitted in the raygen and, when the
  Proceed loop reads them, rebuilt in the generated hit shaders the same way
  `threadId` already was. What a group SHARES, groupshared and barriers, is
  still refused. The divisors are constants of at least 1, so recomputing
  cannot divide by zero.
- Python and C++, byte-identical, including on the real 115 KB Unreal shader
  from the dump, which now validates and signs. `lower.py` gains a numthreads
  reader matching `rq::NumThreads`, and a shader that has none refuses with
  one message in both.
- New case `group`, in both suites: `numthreads(16, 8, 1)` so the axes
  differ, the pixel computed ONLY from the group values, and `SV_GroupIndex`
  gating the commit inside the loop so the any-hit rebuilds it too. Bit-exact
  against WARP, 9031 hits.
- **The first version of that test could not fail, and was caught.** Swapping
  the axes' group sizes diverged by 5999, but a wrong row stride in the
  flattening matched WARP exactly: the gate read bit 2 of `SV_GroupIndex`,
  which comes only from x whatever the stride. It now reads bit 4 XOR bit 2,
  and the same poisoned build diverges by 5398.
- NOT yet run in the game, and NOT yet able to render anything there: see the
  indirect dispatch note in CLAUDE.md. This pass reaches Unreal through a
  compute `DispatchIndirect`, which the shim does not emulate yet.

---

## 0.37.0

- **The GPU crash is fixed, at its cause.** A hit shader READING a cbuffer bound
  by a local root signature makes the Pascal driver access-violate inside
  `CreateStateObject`, intermittently, on a cold compile. That read was how
  the shim delivered `GeometryIndex` and `InstanceContributionToHitGroupIndex`
  to the hit shaders. It is gone.
- **New pass: baking.** `phase5/rewriter/bake.py` and
  `proxy/rewriter/rq_bake.cpp`, byte-identical, take the lowered text and a
  list of (geometryIndex, instanceContribution) pairs, replace every record
  read with an immediate, and emit one copy of every hit shader per pair
  (`AnyHit_k`, `ClosestHit_k`, `Isect_k`, `ClosestHitProc_k`). The record
  type, its global, its resource record and any declaration left unused go
  with it, and the pass refuses its own output if a record read survives.
  `lower()` is unchanged, so nothing that reads no record moves by a byte.
- **The pipeline bakes twice.** The pairs belong to the scene and the scene
  does not exist when the pipeline is created, so the shim bakes `(0, 0)`
  there, which is what the old table held before the first dispatch too. The
  first dispatch that needs a pair it lacks lowers the shader again from the
  original bytes, builds a new state object with one hit group per pair, and
  retires the old one. The pair set only grows, like the table.
- **No local root signature, and hit records are 32 bytes again.** Each record
  points at the hit group copy carrying its own pair.
- Dispatch is now serialised per pipeline under a lock, because a dispatch can
  rebuild both the table and the state object and command lists are recorded
  on several threads.
- `dxrw rewrite` bakes as the proxy does, `0:0` by default or a given list;
  `dxrw bake` and `dxrewrite.py bake` expose the pass. The shape file the
  shim dumps gains `baked=N`, and `sotest` builds a baked library the way the
  shim does.
- Measured, NVIDIA cache cleared before every trial: the fifteen Unreal
  libraries from an Escher run that read record constants crashed at 30 to 80
  percent before, and are **0 of 225** baked. The vendored crasher is 10 of 15
  as it was and 0 of 15 baked, side by side. A real Unreal shader baked with
  27 pairs, what Escher's scene asked for, is 0 of 15.
- New dispatch case `bothgeom`: both kinds, every commit gated on the
  contribution the record reports, so all four hit shaders are copied and
  the pipeline builds a triangle and a procedural group per pair. With every
  record forced onto copy 0 it diverges by 7396, exactly the procedural hit
  count; `geom` and `geomcontrib` diverge by 4624 under the same forcing. The
  per-record copy choice carries the result.
- The rewriter suite gains a bake section: Python and C++ byte-identical, the
  result validates, and no record read survives, on three pair lists.
- The driver-crash repro can now be run from a clone. `*.bin` was ignored, so
  none of the `.rs.bin` root signatures beside the vendored libraries were
  ever committed, and a DXR state object does not build without one.
- Documentation brought up to date:
  - README: the status says plainly that a full Unreal run does not work yet
    and why, the side-effect refusal from 0.36.6 is listed, and the accessor
    paragraph describes the baking instead of the local root signature.
  - docs/usage.md: the five diagnostics are acknowledged instead of "exactly
    six settings, there are no others", `dump` is described as writing lowered
    shaders too, there is a troubleshooting entry for the device being removed
    during pipeline creation, and the Unreal notes state where a real game
    stands, including that a refused shader is fatal to it.
  - dxr-tier-11.example.ini: `rqstub`, `rqphase` and `rqonly` were missing,
    `rqlimit` was described as it worked before 0.36.2, and the whole block had
    been appended to the end of the `dxgi.dll` section. They now have their
    own section, with the Unreal caveat stated once at the top.
  - The Versioning section above no longer says 0.x means the tier flip is
    opt-in, which stopped being true at 0.10.0, and it records which versions
    have tags.
- Tagged `v0.37.0`.

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
