// The shim's copy of an application's top-level structure. See the header.
#include "shim_scene.h"

#include "as_tracker.h"
#include "gpu_hold.h"
#include "proxy_log.h"
#include "shim_scene_cs.h"

#include <wrl/client.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace shimscene {
namespace {

enum Kind { kResult, kScratch, kInstances, kContrib, kUploadInstances, kUploadContrib };
struct Slot {
    ID3D12Device* device;
    Kind kind;
    UINT64 size;
    ComPtr<ID3D12Resource> res;
};
struct Entry {
    Copy copy;
};

std::mutex g_lock;
bool g_active = false;
UINT g_k = 0;
ID3D12Device* g_pipeDevice = nullptr;
ComPtr<ID3D12RootSignature> g_rs;
ComPtr<ID3D12PipelineState> g_pso;
std::vector<Slot> g_pool;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, Copy> g_copies;

bool Latest(ID3D12Resource* r) {
    for (const auto& kv : g_copies)
        if (kv.second.tlasRes == r || kv.second.contribRes == r) return true;
    return false;
}

// Caller holds g_lock. A buffer is free when no GPU work can still read it
// and it is not the latest copy of some structure, which a dispatch recorded
// later may still pick up.
ComPtr<ID3D12Resource> Acquire(ID3D12Device* dev, Kind kind, UINT64 size, const void* owner) {
    for (auto& s : g_pool)
        if (s.device == dev && s.kind == kind && s.size >= size && !gpuhold::Busy(s.res.Get()) &&
            !Latest(s.res.Get())) {
            gpuhold::Use(s.res.Get(), owner);
            return s.res;
        }
    const bool upload = kind == kUploadInstances || kind == kUploadContrib;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = upload ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            upload ? D3D12_RESOURCE_STATE_GENERIC_READ
                   : kind == kResult ? D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
                                     : D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    if (g_pool.size() >= 64)
        for (auto it = g_pool.begin(); it != g_pool.end();)
            if (!gpuhold::Busy(it->res.Get()) && !Latest(it->res.Get())) it = g_pool.erase(it);
            else ++it;
    g_pool.push_back({ dev, kind, size, r });
    gpuhold::Use(r.Get(), owner);
    return r;
}

void Transition(ID3D12GraphicsCommandList4* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &x);
}

}  // namespace

void Activate(UINT k) {
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_active)
        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): the shim's copies of the scene are "
                 "ON from the next top-level build, for up to %u TraceRay argument pair(s)\n", k);
    g_active = true;
    g_k = (std::max)(g_k, k);
}

bool Active() {
    std::lock_guard<std::mutex> g(g_lock);
    return g_active;
}

bool Record(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev,
            const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& app,
            const void* owner, std::string* why) {
    const auto& in = app.Inputs;
    if (in.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL || !in.NumDescs)
        return true;
    if (in.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY) {
        *why = "an array of pointers to instance descriptions";
        return false;
    }
    const UINT gmax = astrack::MaxGeometryCount();
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_active) return true;
    const UINT k = g_k ? g_k : 1;
    const UINT64 stride = (UINT64)k * gmax;
    if ((UINT64)in.NumDescs * stride > 0xFFFFFFull) {
        *why = "the shim's layout needs more than 2^24 hit group records";
        return false;
    }
    if (g_pipeDevice != dev || !g_pso) {
        ComPtr<ID3D12RootSignature> rs;
        ComPtr<ID3D12PipelineState> pso;
        if (FAILED(dev->CreateRootSignature(0, g_shimSceneCS, sizeof(g_shimSceneCS), IID_PPV_ARGS(&rs)))) {
            *why = "could not create the scene copy root signature";
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs.Get();
        pd.CS.pShaderBytecode = g_shimSceneCS;
        pd.CS.BytecodeLength = sizeof(g_shimSceneCS);
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) {
            *why = "could not create the scene copy pipeline";
            return false;
        }
        g_rs = rs; g_pso = pso; g_pipeDevice = dev;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS mine{};
    mine.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    mine.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    mine.NumDescs = in.NumDescs;
    mine.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};
    dev->GetRaytracingAccelerationStructurePrebuildInfo(&mine, &pre);
    if (!pre.ResultDataMaxSizeInBytes) {
        *why = "the driver gave no size for the scene copy";
        return false;
    }
    auto result = Acquire(dev, kResult, pre.ResultDataMaxSizeInBytes, owner);
    auto scratch = Acquire(dev, kScratch, (std::max)(pre.ScratchDataSizeInBytes, (UINT64)256), owner);
    auto inst = Acquire(dev, kInstances, (UINT64)in.NumDescs * 64, owner);
    auto contrib = Acquire(dev, kContrib, (UINT64)in.NumDescs * 4, owner);
    if (!result || !scratch || !inst || !contrib) {
        *why = "could not create the scene copy buffers";
        return false;
    }
    Transition(cl, inst.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cl, contrib.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetComputeRootSignature(g_rs.Get());
    cl->SetPipelineState(g_pso.Get());
    const UINT c[2] = { in.NumDescs, (UINT)stride };
    cl->SetComputeRoot32BitConstants(0, 2, c, 0);
    cl->SetComputeRootShaderResourceView(1, in.InstanceDescs);
    cl->SetComputeRootUnorderedAccessView(2, inst->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(3, contrib->GetGPUVirtualAddress());
    cl->Dispatch((in.NumDescs + 63) / 64, 1, 1);
    Transition(cl, inst.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cl, contrib.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cl, scratch.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    mine.InstanceDescs = inst->GetGPUVirtualAddress();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{};
    b.Inputs = mine;
    b.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    b.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
    D3D12_RESOURCE_BARRIER u{};
    u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    u.UAV.pResource = result.Get();
    cl->ResourceBarrier(1, &u);

    Copy cp;
    cp.tlas = result->GetGPUVirtualAddress();
    cp.contrib = contrib->GetGPUVirtualAddress();
    cp.count = in.NumDescs;
    cp.k = k;
    cp.gmax = gmax;
    cp.tlasRes = result.Get();
    cp.contribRes = contrib.Get();
    g_copies[app.DestAccelerationStructureData] = cp;
    static LONG first = 0;
    if (InterlockedCompareExchange(&first, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): first copy of a scene, %u instances, "
                 "%u records each (%u pair(s) x %u geometries)\n",
                 in.NumDescs, (UINT)stride, k, gmax);
    return true;
}

bool RecordFromSnapshot(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev,
                        D3D12_GPU_VIRTUAL_ADDRESS appTlas,
                        const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& descs,
                        const void* owner, std::string* why) {
    const UINT count = (UINT)descs.size();
    if (!count) { *why = "an empty scene"; return false; }
    const UINT gmax = astrack::MaxGeometryCount();
    std::lock_guard<std::mutex> g(g_lock);
    const UINT k = g_k ? g_k : 1;
    const UINT64 stride = (UINT64)k * gmax;
    if ((UINT64)count * stride > 0xFFFFFFull) {
        *why = "the shim's layout needs more than 2^24 hit group records";
        return false;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS mine{};
    mine.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    mine.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    mine.NumDescs = count;
    mine.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre{};
    dev->GetRaytracingAccelerationStructurePrebuildInfo(&mine, &pre);
    if (!pre.ResultDataMaxSizeInBytes) {
        *why = "the driver gave no size for the scene copy";
        return false;
    }
    auto result = Acquire(dev, kResult, pre.ResultDataMaxSizeInBytes, owner);
    auto scratch = Acquire(dev, kScratch, (std::max)(pre.ScratchDataSizeInBytes, (UINT64)256), owner);
    auto inst = Acquire(dev, kUploadInstances, (UINT64)count * 64, owner);
    auto contrib = Acquire(dev, kUploadContrib, (UINT64)count * 4, owner);
    if (!result || !scratch || !inst || !contrib) {
        *why = "could not create the scene copy buffers";
        return false;
    }
    D3D12_RAYTRACING_INSTANCE_DESC* d = nullptr;
    UINT* c = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(inst->Map(0, &none, reinterpret_cast<void**>(&d))) ||
        FAILED(contrib->Map(0, &none, reinterpret_cast<void**>(&c)))) {
        *why = "could not map the scene copy buffers";
        return false;
    }
    for (UINT i = 0; i < count; ++i) {
        d[i] = descs[i];
        c[i] = descs[i].InstanceContributionToHitGroupIndex;
        d[i].InstanceContributionToHitGroupIndex = (UINT)((i * stride) & 0xFFFFFFu);
    }
    inst->Unmap(0, nullptr);
    contrib->Unmap(0, nullptr);
    Transition(cl, scratch.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    mine.InstanceDescs = inst->GetGPUVirtualAddress();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{};
    b.Inputs = mine;
    b.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    b.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
    D3D12_RESOURCE_BARRIER u{};
    u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    u.UAV.pResource = result.Get();
    cl->ResourceBarrier(1, &u);
    Copy cp;
    cp.tlas = result->GetGPUVirtualAddress();
    cp.contrib = contrib->GetGPUVirtualAddress();
    cp.count = count;
    cp.k = k;
    cp.gmax = gmax;
    cp.tlasRes = result.Get();
    cp.contribRes = contrib.Get();
    g_copies[appTlas] = cp;
    ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): scene copy built from the snapshot of the "
             "structure's latest build, %u instances\n", count);
    return true;
}

bool Lookup(D3D12_GPU_VIRTUAL_ADDRESS appTlas, Copy* out) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_copies.find(appTlas);
    if (it == g_copies.end()) return false;
    *out = it->second;
    return true;
}

}  // namespace shimscene
