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
#pragma once

#include <d3d12.h>

#include <string>
#include <vector>

namespace shimscene {

// Switch the copies on, for pipelines tracing with up to `k` argument pairs.
void Activate(UINT k);
bool Active();

// After the application's top-level build is recorded into `cl`: records the
// copy. Replaces the compute root signature and pipeline; the CALLER restores
// them. `owner` is the recording list.
bool Record(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev,
            const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& app,
            const void* owner, std::string* why);

// The same, built from the instance descriptions a structure's latest build
// used, kept on the CPU (astrack::InstanceSnapshot): for a pipeline created
// after its scene was built, when the scene is not built again. Exact, since
// the snapshot is that build's own data.
bool RecordFromSnapshot(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev,
                        D3D12_GPU_VIRTUAL_ADDRESS appTlas,
                        const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& descs,
                        const void* owner, std::string* why);

struct Copy {
    D3D12_GPU_VIRTUAL_ADDRESS tlas = 0;     // the shim's structure
    D3D12_GPU_VIRTUAL_ADDRESS contrib = 0;  // the application's contributions, one UINT per instance
    UINT count = 0, k = 0, gmax = 0;
    const void* tlasRes = nullptr;          // for gpu_hold
    const void* contribRes = nullptr;
};
// The latest copy of the application structure at `appTlas`.
bool Lookup(D3D12_GPU_VIRTUAL_ADDRESS appTlas, Copy* out);

}  // namespace shimscene
