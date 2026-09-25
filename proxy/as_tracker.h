// What the shim needs to know about the application's acceleration structures,
// and why.
//
// The shim builds the shader table for a lowered RayQuery shader, and a record
// in that table has to match the geometry that resolves to it:
//
//   index = RayContributionToHitGroupIndex
//         + MultiplierForGeometryContributionToShaderIndex * GeometryIndex
//         + InstanceContributionToHitGroupIndex
//
// The first two are ours, in the DispatchRays call. The third is the
// APPLICATION'S, baked into the instance descriptions when it built its
// top-level structure. That single fact is behind both open problems:
//
//   - a query committing BOTH triangle and procedural hits needs records of
//     different TYPES at different indices, and which index a geometry lands on
//     depends on the app's instances;
//   - even for triangles ALONE, the shim currently builds one record and
//     assumes every instance contributes 0. True of every scene tested here,
//     and not guaranteed of a real one.
//
// The two halves of the answer are very different in cost.
//
// BLAS geometry types are FREE. D3D12_RAYTRACING_GEOMETRY_DESC arrives as CPU
// memory in the build call, so the type of every geometry can simply be read
// and remembered. That is what this file does.
//
// TLAS instance data is NOT free. InstanceDescs is a GPU virtual address, so
// the contributions can only be read back after the build has run, which means
// a copy and a sync per top-level build, in an engine that rebuilds its TLAS
// every frame. See the notes in docs/phase5-dxil-recon.md.
//
// The first step of that half is here: a copy needs a RESOURCE and D3D12 has no
// way to turn an address back into one, so the top-level branch asks
// res_tracker, which remembers every buffer the application created through the
// wrapped device. Whether that lookup succeeds is recorded and logged, because
// if it does not the whole approach is dead and it should say so.
#pragma once

#include <d3d12.h>

#include <string>
#include <vector>

namespace astrack {

// What a bottom-level structure holds. A BLAS carries ONE geometry type: the
// NVIDIA driver returns zero-sized prebuild info for a mixed one, which the
// Phase 4 probe found the hard way.
enum class Kind { kUnknown, kTriangles, kProcedural, kMixed };

struct BlasInfo {
    Kind kind = Kind::kUnknown;
    UINT geometryCount = 0;
    // How the shim learned it: 0 built, 1 CLONE or COMPACT of a known one,
    // 2 DESERIALIZE, 3 a copy of something unknown. Said when refusing.
    uint8_t origin = 0;
};

// Remember what a bottom-level build contains. Safe to call for any build;
// top-level ones and unreadable layouts are ignored.
void NoteBuild(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc);

// NO_DUPLICATE_ANYHIT_INVOCATION on every geometry of a BOTTOM-level build.
//
// A lowered Proceed loop that appends to a buffer is only the same append
// when the any-hit it becomes runs once per candidate, as Proceed() yields
// each candidate once; without this flag the spec lets an any-hit run more
// than once for one intersection. The flag lives in the geometry
// descriptions, CPU memory in the build call, and it has to be set in the
// PREBUILD query too, with the same inputs, or the buffers the application
// sized would not be the ones the build needs.
//
// Returns `in` when nothing changes (a top-level build, or every geometry
// already flagged), otherwise `copy`, filled in and pointing at `storage`.
// An array of pointers becomes a plain array, the same geometry. Applies to
// the application's own DXR 1.0 any-hit shaders too, which the spec already
// allows to see exactly this behaviour; slower, not wrong.
const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* NoDuplicateAnyHit(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* in,
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* copy,
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>* storage);

// The most geometries any bottom-level structure seen so far holds, at least 1.
UINT MaxGeometryCount();

// What was recorded for the structure at this address, or kUnknown.
BlasInfo Lookup(D3D12_GPU_VIRTUAL_ADDRESS address);

// What a TOP-level structure turned out to contain, once its instance
// descriptions were read. `valid` is false until then.
// Which geometry reaches a hit group record index. A record is triangles or
// procedural, never both, so an index reached by both kinds cannot be served
// by any single record.
enum Reach : uint8_t { kReachNone = 0, kReachTriangles = 1, kReachProcedural = 2 };

// What a hit group record has to tell the shader about itself.
//
// GeometryIndex() in a DXR 1.0 hit shader is a Tier 1.1 feature and
// CreateStateObject refuses it on this hardware, and HLSL exposes no hit-shader
// intrinsic for the instance contribution at all. Both answers are known HERE,
// while the table is being built, so they travel in the record as local root
// signature constants and the hit shader reads them back. Measured at
// SFI0 = 0x0: see phase5/cases/reference/lib_localroot_ref.hlsl.
struct RecordConstants {
    UINT geometryIndex = 0;
    UINT instanceContribution = 0;
    bool assigned = false;
};

struct TlasInfo {
    bool valid = false;
    UINT instanceCount = 0;
    // The largest InstanceContributionToHitGroupIndex any instance carries.
    // The shader table has to be at least this much bigger than one record.
    UINT maxContribution = 0;
    // How many records the scene needs, which with
    // MultiplierForGeometryContributionToShaderIndex = 1 is
    // max(contribution + geometryCount) and NOT maxContribution + 1: each
    // instance occupies one record per geometry in its structure.
    UINT recordCount = 0;
    // Per record, the two numbers the hit shader will read back.
    std::vector<RecordConstants> constants;
    // Two instances whose (contribution, geometry) pairs land on the SAME
    // record but disagree about what it means. One record cannot answer twice,
    // so this is refused rather than answered wrongly. It does not arise in the
    // usual layout, where an engine assigns contributions as a running sum of
    // geometry counts precisely so records do not collide.
    bool constantsConflict = false;
    UINT conflictSlot = 0;
    // Who collides there, for the log: the pair already on the record, and
    // the instance that brought the other one, with its structure.
    std::string conflictDetail;
    // Which geometry types the instances actually reach. Both true is the
    // mixed case the shim cannot yet build a table for.
    bool anyTriangles = false;
    bool anyProcedural = false;
    // Instances whose bottom-level structure was never seen being built, so
    // its type is not known. Nonzero means the answer above is incomplete.
    UINT unknownBlas = 0;
    std::string unknownDetail;   // the first such instance, and why it is unknown
    // What reaches each record index, one entry per record. This is what lets
    // the table carry a record of the right TYPE at each slot rather than one
    // record everywhere.
    std::vector<uint8_t> reach;
    // Every distinct (InstanceContributionToHitGroupIndex, geometry count)
    // the instances carry, sorted. What an APPLICATION's own table needs to
    // answer GeometryIndex(), for whatever TraceRay arguments it uses; see
    // GeometryLabels.
    std::vector<std::pair<UINT, UINT>> classes;
};

// GeometryIndex() in an application's own hit shaders: which geometry index a
// hit on each of the first `records` records of its hit group table has, when
// its TraceRay calls use these (RayContributionToHitGroupIndex,
// MultiplierForGeometryContributionToHitGroupIndex) pairs, low 4 bits each as
// DXR uses them. Over every live top-level structure read so far.
//
//   >= 0  that geometry index
//   -1    nothing reaches the record
//   -2    two different geometry indices reach it: one record cannot answer
//         both, and the application's layout has to be replaced
//
// With `exact`, `bound` are exactly the scenes the dispatch traces (resolved
// through its root signature, gidx::ResolveScenes) and only they count;
// `*read` is false unless one of them has been read. Otherwise `bound` is
// empty (since 0.48.0; it used to be the bound root SRVs) and every live one
// counts, which can only find MORE collisions, and `*read` is false when no
// top-level structure has been read at all. `*stale` is set when a scene
// that counted was read from an OLDER build than its latest: GPU-written
// instances are read again only every few builds, and a submission late.
std::vector<int32_t> GeometryLabels(const std::vector<std::pair<UINT, UINT>>& traceArgs,
                                    UINT records,
                                    const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& bound, bool exact,
                                    bool* read, bool* stale);

// The same scenes, for the shim's own layout: per pair, an id it shares with
// every pair that puts each (instance, geometry) on the same record below
// `records`, so both can share the shim's records too; -1 for a pair that
// puts none below it, which no hit can use (0.57.0).
std::vector<int> PairClasses(const std::vector<std::pair<UINT, UINT>>& pairs, UINT records,
                             const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& bound, bool exact);

// The instance descriptions of a top-level build, read from CPU-visible memory
// at record time. Cheap path: no copy, no sync. They are also KEPT, as the
// snapshot below, until the structure is built again.
void NoteInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                   const D3D12_RAYTRACING_INSTANCE_DESC* descs, UINT count);

// A top-level build with NO instances: known without reading anything, every
// ray misses. Until 0.52.2 such a build was never read, so a dispatch on it
// was refused and drew nothing, where it should have drawn all misses (the
// first frame of Escher's open world).
void NoteEmpty(D3D12_GPU_VIRTUAL_ADDRESS tlas);

// Exactly the instance descriptions the LATEST build of this structure used,
// when they were CPU-visible; false otherwise. So a copy of the scene can be
// built after the fact, for a pipeline created after its structure. Every
// top-level build drops the old one first (DropSnapshot), so a snapshot is
// never older than the structure it stands for.
bool InstanceSnapshot(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                      std::vector<D3D12_RAYTRACING_INSTANCE_DESC>* out);
void DropSnapshot(D3D12_GPU_VIRTUAL_ADDRESS tlas);

// A GPU copy of those descriptions, recorded into the application's own list
// but not readable until it has run. Since 0.52.0 every build with GPU-
// written instances has one (shimscene::Save), carrying the build it copies. `owner` is the list that
// recorded the copy: only ITS submission may stamp the read, see AfterSubmit.
void NotePendingInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                          ID3D12Resource* readback, UINT count,
                          const void* owner, ID3D12Resource* keepAlive = nullptr);

// Called by the queue hook after every submission, with the lists it carried
// (the application's pointers, wrapped or not). Stamps the reads those lists
// recorded with a fence signalled on `queue`, and parses anything the GPU is
// already past. Never blocks: the answer arrives a submission late rather than
// stalling for it.
//
// ONLY the submitted lists' reads are stamped. Applications record on several
// threads, so a read recorded into a list that is still OPEN must not be
// stamped by some other thread's submission: the fence would pass, the shim
// would drop the readback buffer, and the open list would then reference a
// deleted resource. Its Close fails with E_INVALIDARG, which Unreal makes
// fatal. That is what 0.38.0 and earlier did.
void AfterSubmit(ID3D12CommandQueue* queue,
                 ID3D12CommandList* const* lists, UINT count);

// The list was reset or destroyed. Reads it recorded and never submitted will
// never run, so they are dropped. Stamped reads are left alone.
void DropUnsubmitted(const void* owner);

// What is known about the top-level structure at this address.
TlasInfo LookupTlas(D3D12_GPU_VIRTUAL_ADDRESS address);

// Which build of this structure is the latest, as a serial comparable with
// what a read carries; 0 when it was never seen being built.
UINT64 LatestBuild(D3D12_GPU_VIRTUAL_ADDRESS tlas);

// The serial the next top-level build will exceed: a build whose LatestBuild
// is at most this was recorded before now.
UINT64 SerialNow();

// Were these structures' instances read from their LATEST builds? A table
// built from an older read can be wrong for the scene as it is now: that was
// the RayQuery path until 0.52.0, drawn wrong after an in-place rebuild.
bool Current(const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& scenes);
// The same, over every live structure that has been read.
bool LiveCurrent();

// At a split, once the GPU has run a segment: make what is known about `tlas`
// the instances of build `build`, reading them back now if that build's read
// is still pending. Its read has run if `list` (the list being split) recorded
// it, or its fence has passed. False when it cannot be had.
bool BringToBuild(D3D12_GPU_VIRTUAL_ADDRESS tlas, UINT64 build, const void* list);

// CopyRaytracingAccelerationStructure (0.53.0). CLONE and COMPACT make the
// same structure: a bottom-level one keeps its geometry, a top-level one its
// instances, as of the source's latest build (a read still pending there is
// followed at a split). Any other copy into `dst` leaves nothing known about
// it: a DESERIALIZED structure's geometry is inside a driver-opaque blob, so
// an instance on one is refused, and a dispatch on a deserialized top-level
// one is refused as not read. Until 0.53.0 copies were not followed at all:
// an instance on a compacted structure counted one geometry of unknown kind,
// silently, and a copy over a known address kept the old answer.
void NoteCopy(D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src,
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode);

// Does a scene the dispatch may trace have an instance on a bottom-level
// structure of unknown geometry? Same selection as GeometryLabels.
bool UnknownBlas(const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& bound, bool exact);

// Should the instance data of this top-level build be read? Called once per
// build, AFTER NoteBuild. Up to 0.39.0 the answer was "once per destination
// address", and an engine that rebuilds a structure in place when a level
// loads kept the FIRST answer forever: the Escher open world dispatched
// against a 27-record table while reaching record 1998, and the GPU hung.
//
// `cheap` is true when the descriptions are CPU-visible, and then the answer
// is always yes: reading them costs nothing. Otherwise, a GPU copy, it is yes
// when the structure has not been read, when its instance count changed, or
// every kRereadEvery builds of it, and never while a copy of it is pending.
bool WantInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas, UINT numDescs, bool cheap);

// Would the shim's shader table be wrong for the scene as read so far?
//
// The shim builds ONE hit group, and the two directions of mismatch between it
// and the scene's geometry are NOT symmetric. Both were measured on a scene
// holding one triangle instance and one procedural instance:
//
//   triangle-only shader, procedural geometry present
//       the procedural geometry reports no hit, which is the same answer the
//       shader gives on Tier 1.1, where it never commits a procedural
//       candidate either. 14450 hits, bit-exact against WARP. SAFE.
//
//   procedural shader, triangle geometry reachable
//       triangle hits run the closest-hit anyway, and it labels them
//       procedural, so they are committed when they should not be. 15418
//       against WARP's 7396, the 8022 difference being exactly the triangle
//       hits. WRONG, and silently.
//
// So `shaderCommitsProcedural` decides the question, not whether the scene
// happens to be mixed. Fills `why` and returns true when the dispatch must
// not run.
//
// NOT settled by the debug layer. It is silent on BOTH cases, including the
// one measured to be wrong, so it does not police hit group and geometry type
// agreement and its silence says nothing. The safe direction above rests on
// measurement against WARP on one driver, not on the specification.
//
// With `only`, judged over exactly those structures: the scene the dispatch
// traces, resolved through its root signature (gidx::ResolveScenes, 0.49.0).
// Without, over EVERY live structure read, which can refuse a dispatch that
// would in fact have been fine. Refusing is the safe direction.
//
// It can also only see what has been READ. When the descriptions live in GPU
// memory the answer arrives a submission late, so the first dispatch of a run
// may not be covered.
bool TableWouldBeWrong(bool shaderCommitsProcedural, std::string* why,
                       const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* only = nullptr);

// True when at least one of `scenes` is a top-level structure whose instance
// data has been read.
bool AnyRead(const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& scenes);

// The live top-level structures in one line, for the periodic stats line:
// address, instances, records, and how many top-level builds ago each was
// last rebuilt. Up to eight.
std::string DescribeLive();

// Only LIVE structures count below: ones rebuilt within the last kLiveWindow
// top-level builds, and not SUPERSEDED: replaced by a structure first built
// after its last build and since built four times. A structure an engine
// stopped rebuilding, the menu's after a level load or the old buffer after
// the scene moves to a bigger one, stops counting, so its records can neither
// collide with the new scene's nor lend it their constants. An application
// that builds one
// structure once and never again keeps it live for as long as it builds
// nothing else, which covers a static scene.
//
// What reaches each hit group record index, aggregated over every top-level
// structure read. The result's size is the number of records the table needs;
// empty means nothing has been read and one record will do.
//
// With `only`, over exactly those structures, the scene the dispatch traces.
// Without, aggregated over every live one: that can only ever ask for MORE
// records of MORE types than one scene needs, which costs a few wasted slots
// and never a wrong one.
std::vector<uint8_t> RecordKinds(const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* only = nullptr);

// The per-record constants, merged over every top-level structure read so far,
// sized to the largest. Slots nothing reaches keep {0,0}, which is harmless:
// a record nothing resolves to is never executed.
std::vector<RecordConstants> RecordConstantsTable(
    const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* only = nullptr);

// How many bottom-level structures have been seen, and of what kinds. For the
// log and for tests, so the tracking can be shown to work before anything
// depends on it.
struct Summary {
    size_t blasCount = 0;
    size_t triangles = 0;
    size_t procedural = 0;
    size_t mixed = 0;
    size_t tlasCount = 0;
    // Of those, how many had their instance buffer resolve to a tracked
    // resource. This is the gate on reading the contributions at all.
    size_t tlasResolved = 0;
    // And how many had their instance descriptions actually parsed.
    size_t tlasRead = 0;
    // The largest contribution index seen across every structure. Zero after a
    // scene has been read means the shim's one-record table is right for it.
    UINT   maxContribution = 0;
    // Set when a top-level build was seen, because its instance data is the
    // half this cannot read.
    bool sawTopLevel = false;
};
Summary GetSummary();

const char* KindName(Kind k);

}  // namespace astrack
