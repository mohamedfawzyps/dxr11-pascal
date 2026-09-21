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
// every frame. That half is deliberately not attempted here; see the notes in
// docs/phase5-dxil-recon.md.
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

// How many bottom-level structures have been seen, and of what kinds. For the
// log and for tests, so the tracking can be shown to work before anything
// depends on it.
struct Summary {
    size_t blasCount = 0;
    size_t triangles = 0;
    size_t procedural = 0;
    size_t mixed = 0;
    size_t tlasCount = 0;
    // Set when a top-level build was seen, because its instance data is the
    // half this cannot read.
    bool sawTopLevel = false;
};
Summary GetSummary();

const char* KindName(Kind k);

}  // namespace astrack
