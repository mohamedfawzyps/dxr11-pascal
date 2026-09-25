// The shim's copy of an application's top-level acceleration structure.
//
// For GeometryIndex() when one hit group record is reached by several
// geometries (see proxy/geom_index_so.h): a record index is computed by the
// hardware as
//
//   RayContributionToHitGroupIndex + Multiplier * GeometryIndex
//     + InstanceContributionToHitGroupIndex
//
// and only the last term comes from the scene. So the shim builds its own
// copy of the scene, the same instances, transforms, masks, flags, IDs and
// bottom-level structures, with each instance's contribution replaced by
// instance * stride, stride = k * the most geometries any structure holds.
// A variant pipeline traces the copy with (q, k), q the TraceRay argument
// pair, so every (instance, geometry, pair) lands on a record of its own.
//
// Built right after every application top-level build while switched on, in
// the same command list, from the same instance buffer: so the copy is always
// exactly the scene the application just built, moving objects included. It
// costs one small pass and one extra top-level build per application build,
// and only once a pipeline needs it; nothing needs it for Unreal's layout.
//
// A copy can also be needed where none was built then: the pipeline came
// after the scene, or wants more argument pairs. So every top-level build
// whose instances the GPU writes also gets them SAVED, copied verbatim into a
// buffer of the shim's (one small pass, whether or not anything needs it),
// and a dispatch builds the copy from that, in its own command list (0.51.0).
// Instances the CPU can see are kept on the CPU instead
// (astrack::InstanceSnapshot). Until 0.51.0 the first case was refused, and
// for a scene built once, never drawn.
#pragma once

#include <d3d12.h>

#include <string>
#include <vector>

namespace shimscene {

// Switch the copies on, for pipelines tracing with up to `k` argument pairs.
void Activate(UINT k);
bool Active();

// Every top-level build, before Save and Record: whatever the shim held for
// the structure's previous build no longer stands for it.
void NoteBuild(D3D12_GPU_VIRTUAL_ADDRESS appTlas);

// CopyRaytracingAccelerationStructure into `dst`: a new build of it, which
// with `same` (CLONE or COMPACT) of a structure known here is the same scene
// as the source's latest build (0.53.0).
void NoteCopy(D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src, bool same);

// After the application's top-level build is recorded into `cl`, when its
// instances are in GPU-only memory: records the verbatim copy, and a copy of
// that into a new readback buffer, returned with a reference in *readback
// (null if it could not be made). Replaces the compute root signature and
// pipeline; the CALLER restores them. `owner` is the recording list.
bool Save(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev,
          const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& app,
          const void* owner, ID3D12Resource** readback, std::string* why);

// After the application's top-level build is recorded into `cl`, while
// switched on: records the copy. Same contract as Save.
bool Record(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev,
            const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& app,
            const void* owner, std::string* why);

struct Copy {
    D3D12_GPU_VIRTUAL_ADDRESS tlas = 0;     // the shim's structure
    D3D12_GPU_VIRTUAL_ADDRESS contrib = 0;  // the application's contributions, one UINT per instance
    UINT count = 0, k = 0, gmax = 0;
    const void* tlasRes = nullptr;          // for gpu_hold
    const void* contribRes = nullptr;
};

// A copy of the latest build of `appTlas`, for at least `k` argument pairs,
// that a dispatch recorded into `owner` may trace: the one built with that
// build, or one this list built already, or a new one recorded into `cl`
// from the saved instances or the CPU snapshot. A copy recorded here is this
// list's alone: another list may run first. May replace the compute root
// signature and pipeline; the CALLER restores them.
bool Ensure(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, D3D12_GPU_VIRTUAL_ADDRESS appTlas,
            UINT k, const void* owner, Copy* out, std::string* why);

// The list was reset or destroyed: the copies it built are forgotten.
void DropOwner(const void* owner);

}  // namespace shimscene
