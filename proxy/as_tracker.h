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
};

// Remember what a bottom-level build contains. Safe to call for any build;
// top-level ones and unreadable layouts are ignored.
void NoteBuild(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc);

// What was recorded for the structure at this address, or kUnknown.
BlasInfo Lookup(D3D12_GPU_VIRTUAL_ADDRESS address);

// What a TOP-level structure turned out to contain, once its instance
// descriptions were read. `valid` is false until then.
struct TlasInfo {
    bool valid = false;
    UINT instanceCount = 0;
    // The largest InstanceContributionToHitGroupIndex any instance carries.
    // The shader table has to be at least this much bigger than one record.
    UINT maxContribution = 0;
    // Which geometry types the instances actually reach. Both true is the
    // mixed case the shim cannot yet build a table for.
    bool anyTriangles = false;
    bool anyProcedural = false;
    // Instances whose bottom-level structure was never seen being built, so
    // its type is not known. Nonzero means the answer above is incomplete.
    UINT unknownBlas = 0;
};

// The instance descriptions of a top-level build, read from CPU-visible memory
// at record time. Cheap path: no copy, no sync.
void NoteInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                   const D3D12_RAYTRACING_INSTANCE_DESC* descs, UINT count);

// A GPU copy of those descriptions, recorded into the application's own list
// but not readable until it has run. Expensive path.
void NotePendingInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                          ID3D12Resource* readback, UINT count);

// Called by the queue hook after every submission. Stamps anything pending
// with a fence signalled on `queue`, and parses anything the GPU is already
// past. Never blocks: the cost of reading the instance data is one fence
// signal, and the answer arrives a submission late rather than stalling for it.
void AfterSubmit(ID3D12CommandQueue* queue);

// What is known about the top-level structure at this address.
TlasInfo LookupTlas(D3D12_GPU_VIRTUAL_ADDRESS address);

// Would the shim's shader table be wrong for the scene as read so far? A
// lowered RayQuery dispatch builds ONE hit group record of ONE geometry type,
// which is only right when every instance contributes 0 and they all reach the
// same kind of geometry. Fills `why` and returns true when it does not hold.
//
// Judged over EVERY top-level structure read, not the one the shader is about
// to trace against, because which structure that is is not knowable here when
// it arrives through a descriptor table. So this can refuse a dispatch that
// would in fact have been fine. Refusing is the safe direction: the
// alternative is a silently wrong image.
//
// It can also only see what has been READ. When the descriptions live in GPU
// memory the answer arrives a submission late, so the first dispatch of a run
// may not be covered.
bool TableWouldBeWrong(std::string* why);

// Would the shim's shader table be wrong for the scene as read so far? A
// lowered RayQuery dispatch builds ONE hit group record of ONE geometry type,
// which is only right when every instance contributes 0 and they all reach the
// same kind of geometry. Fills `why` and returns true when it does not hold.
//
// Judged over EVERY top-level structure read, not the one the shader is about
// to trace against, because which structure that is is not knowable here when
// it arrives through a descriptor table. So this can refuse a dispatch that
// would in fact have been fine. Refusing is the safe direction: the
// alternative is a silently wrong image.
//
// It can also only see what has been READ. When the descriptions live in GPU
// memory the answer arrives a submission late, so the first dispatch of a run
// may not be covered.
bool TableWouldBeWrong(std::string* why);

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
