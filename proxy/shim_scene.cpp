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
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace shimscene {
namespace {

enum Kind { kResult, kScratch, kInstances, kContrib, kUploadInstances, kUploadContrib, kSaved };
struct Slot {
    ID3D12Device* device;
    Kind kind;
    UINT64 size;
    ComPtr<ID3D12Resource> res;
};
// A copy, and which build of the application's structure it stands for.
struct Held {
    Copy copy;
    UINT64 build = 0;
};
// A build's GPU-written instances, copied verbatim.
struct Saved {
    ID3D12Resource* res = nullptr;   // owned by g_pool
    UINT count = 0;
    UINT64 build = 0;
};

std::mutex g_lock;
bool g_active = false;
UINT g_k = 0;
ID3D12Device* g_pipeDevice = nullptr;
ComPtr<ID3D12RootSignature> g_rs;
ComPtr<ID3D12PipelineState> g_pso;
std::vector<Slot> g_pool;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT64> g_builds;                    // builds of each structure
// The most geometries any bottom-level structure had when each top-level
// build was RECORDED. A copy made later from that build's instances never
// uses fewer: a structure streamed out and its address reused for a smaller
// one would otherwise shrink every instance's block of records (0.52.3).
std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT> g_buildGmax;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, Held> g_copies;                       // built with the build
std::map<std::pair<const void*, D3D12_GPU_VIRTUAL_ADDRESS>, Held> g_local;  // built at a dispatch
std::map<D3D12_GPU_VIRTUAL_ADDRESS, Saved> g_saved;

bool Latest(ID3D12Resource* r) {
    for (const auto& kv : g_copies)
        if (kv.second.copy.tlasRes == r || kv.second.copy.contribRes == r) return true;
    for (const auto& kv : g_local)
        if (kv.second.copy.tlasRes == r || kv.second.copy.contribRes == r) return true;
    for (const auto& kv : g_saved)
        if (kv.second.res == r) return true;
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

// Caller holds g_lock.
bool EnsurePipeLocked(ID3D12Device5* dev, std::string* why) {
    if (g_pipeDevice == dev && g_pso) return true;
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
    return true;
}

// Caller holds g_lock. The shim's structure from `count` instance
// descriptions at `src` (GPU memory, readable as an SRV), recorded into `cl`.
bool BuildLocked(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, D3D12_GPU_VIRTUAL_ADDRESS src,
                 UINT count, UINT k, UINT gmaxFloor, const void* owner, Copy* out,
                 std::string* why) {
    const UINT gmax = (std::max)(astrack::MaxGeometryCount(), gmaxFloor);
    const UINT64 stride = (UINT64)k * gmax;
    if ((UINT64)count * stride > 0xFFFFFFull) {
        *why = "the shim's layout needs more than 2^24 hit group records";
        return false;
    }
    if (!EnsurePipeLocked(dev, why)) return false;
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
    auto inst = Acquire(dev, kInstances, (UINT64)count * 64, owner);
    auto contrib = Acquire(dev, kContrib, (UINT64)count * 4, owner);
    if (!result || !scratch || !inst || !contrib) {
        *why = "could not create the scene copy buffers";
        return false;
    }
    Transition(cl, inst.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cl, contrib.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetComputeRootSignature(g_rs.Get());
    cl->SetPipelineState(g_pso.Get());
    const UINT c[3] = { count, (UINT)stride, 0 };
    cl->SetComputeRoot32BitConstants(0, 3, c, 0);
    cl->SetComputeRootShaderResourceView(1, src);
    cl->SetComputeRootUnorderedAccessView(2, inst->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(3, contrib->GetGPUVirtualAddress());
    cl->Dispatch((count + 63) / 64, 1, 1);
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
    out->tlas = result->GetGPUVirtualAddress();
    out->contrib = contrib->GetGPUVirtualAddress();
    out->count = count;
    out->k = k;
    out->gmax = gmax;
    out->tlasRes = result.Get();
    out->contribRes = contrib.Get();
    return true;
}

// Caller holds g_lock. The same, from instance descriptions on the CPU.
bool BuildFromCpuLocked(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev,
                        const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& descs, UINT k,
                        UINT gmaxFloor, const void* owner, Copy* out, std::string* why) {
    const UINT count = (UINT)descs.size();
    if (!count) { *why = "an empty scene"; return false; }
    const UINT gmax = (std::max)(astrack::MaxGeometryCount(), gmaxFloor);
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
    out->tlas = result->GetGPUVirtualAddress();
    out->contrib = contrib->GetGPUVirtualAddress();
    out->count = count;
    out->k = k;
    out->gmax = gmax;
    out->tlasRes = result.Get();
    out->contribRes = contrib.Get();
    return true;
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

void NoteBuild(D3D12_GPU_VIRTUAL_ADDRESS appTlas) {
    const UINT gmax = astrack::MaxGeometryCount();
    std::lock_guard<std::mutex> g(g_lock);
    ++g_builds[appTlas];
    g_buildGmax[appTlas] = gmax;
    g_copies.erase(appTlas);
    g_saved.erase(appTlas);
}

bool Save(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev,
          const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& app,
          const void* owner, ID3D12Resource** readback, std::string* why) {
    const auto& in = app.Inputs;
    if (in.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL || !in.NumDescs ||
        !in.InstanceDescs)
        return true;
    if (in.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY) {
        *why = "an array of pointers to instance descriptions";
        return false;
    }
    std::lock_guard<std::mutex> g(g_lock);
    if (!EnsurePipeLocked(dev, why)) return false;
    auto saved = Acquire(dev, kSaved, (UINT64)in.NumDescs * 64, owner);
    if (!saved) {
        *why = "could not create the saved instance buffer";
        return false;
    }
    Transition(cl, saved.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->SetComputeRootSignature(g_rs.Get());
    cl->SetPipelineState(g_pso.Get());
    const UINT c[3] = { in.NumDescs, 0, 1 };
    cl->SetComputeRoot32BitConstants(0, 3, c, 0);
    cl->SetComputeRootShaderResourceView(1, in.InstanceDescs);
    cl->SetComputeRootUnorderedAccessView(2, saved->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(3, saved->GetGPUVirtualAddress());  // unwritten
    cl->Dispatch((in.NumDescs + 63) / 64, 1, 1);
    // And a copy the CPU can read once this has run, for the table of a
    // lowered RayQuery dispatch (0.52.0). Its own buffer each time: the
    // tracker holds it until it is read.
    const UINT64 bytes = (UINT64)in.NumDescs * 64;
    ComPtr<ID3D12Resource> rb;
    if (readback) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
            rb.Reset();
    }
    if (rb) {
        Transition(cl, saved.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyBufferRegion(rb.Get(), 0, saved.Get(), 0, bytes);
        Transition(cl, saved.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        *readback = rb.Detach();
    } else {
        Transition(cl, saved.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    Saved s;
    s.res = saved.Get();
    s.count = in.NumDescs;
    s.build = g_builds[app.DestAccelerationStructureData];
    g_saved[app.DestAccelerationStructureData] = s;
    return true;
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
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_active) return true;
    Held h;
    h.build = g_builds[app.DestAccelerationStructureData];
    if (!BuildLocked(cl, dev, in.InstanceDescs, in.NumDescs, g_k ? g_k : 1,
                     g_buildGmax[app.DestAccelerationStructureData], owner, &h.copy, why))
        return false;
    g_copies[app.DestAccelerationStructureData] = h;
    static LONG first = 0;
    if (InterlockedCompareExchange(&first, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): first copy of a scene, %u instances, "
                 "%u records each (%u pair(s) x %u geometries)\n",
                 in.NumDescs, h.copy.k * h.copy.gmax, h.copy.k, h.copy.gmax);
    return true;
}

bool Ensure(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, D3D12_GPU_VIRTUAL_ADDRESS appTlas,
            UINT k, const void* owner, Copy* out, std::string* why) {
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> snap;
    const bool haveSnap = astrack::InstanceSnapshot(appTlas, &snap);
    std::lock_guard<std::mutex> g(g_lock);
    auto b = g_builds.find(appTlas);
    if (b == g_builds.end()) {
        *why = "the scene this dispatch traces was never seen being built";
        return false;
    }
    const UINT64 build = b->second;
    auto c = g_copies.find(appTlas);
    if (c != g_copies.end() && c->second.build == build && c->second.copy.k >= k) {
        *out = c->second.copy;
        return true;
    }
    const auto key = std::make_pair(owner, appTlas);
    auto l = g_local.find(key);
    if (l != g_local.end() && l->second.build == build && l->second.copy.k >= k) {
        *out = l->second.copy;
        return true;
    }
    const UINT kk = (std::max)(k, g_k ? g_k : 1u);
    Held h;
    h.build = build;
    auto s = g_saved.find(appTlas);
    bool ok = false;
    const char* from = "";
    if (s != g_saved.end() && s->second.build == build) {
        gpuhold::Use(s->second.res, owner);
        ok = BuildLocked(cl, dev, s->second.res->GetGPUVirtualAddress(), s->second.count, kk,
                         g_buildGmax[appTlas], owner, &h.copy, why);
        from = "the saved instances";
    } else if (haveSnap) {
        ok = BuildFromCpuLocked(cl, dev, snap, kk, g_buildGmax[appTlas], owner, &h.copy, why);
        from = "the CPU snapshot";
    } else {
        *why = "neither the instances nor a snapshot of the scene's latest build were kept "
               "(an array of pointers, or instances the shim could not save)";
        return false;
    }
    if (!ok) return false;
    g_local[key] = h;
    *out = h.copy;
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 4)
        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): scene copy built at a dispatch from %s, "
                 "%u instances, %u pair(s)\n", from, h.copy.count, h.copy.k);
    return true;
}

void DropOwner(const void* owner) {
    std::lock_guard<std::mutex> g(g_lock);
    for (auto it = g_local.begin(); it != g_local.end();)
        if (it->first.first == owner) it = g_local.erase(it);
        else ++it;
}

}  // namespace shimscene
