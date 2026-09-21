#include "as_tracker.h"

#include "proxy_log.h"
#include "res_tracker.h"

#include <wrl/client.h>

#include <map>
#include <string>
#include <vector>
#include <mutex>

namespace astrack {
namespace {

std::mutex g_lock;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, BlasInfo> g_blas;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, TlasInfo> g_tlas;
Summary g_summary;

// A recorded copy of a TLAS's instance descriptions, waiting for the GPU.
struct PendingRead {
    D3D12_GPU_VIRTUAL_ADDRESS tlas = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    UINT   count = 0;
    UINT64 fenceValue = 0;   // 0 until a submission stamps it
};
std::vector<PendingRead> g_pending;

// One fence for all of it, created from the first readback buffer's device.
// Single adapter is assumed, which is true of everything this shim targets.
Microsoft::WRL::ComPtr<ID3D12Fence> g_fence;
UINT64 g_fenceValue = 0;

// Fills in a TlasInfo from the descriptions themselves. Caller holds g_lock,
// because this reads g_blas to learn what each instance points at.
void ParseLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                 const D3D12_RAYTRACING_INSTANCE_DESC* d, UINT count) {
    TlasInfo t;
    t.valid = true;
    t.instanceCount = count;
    for (UINT i = 0; i < count; ++i) {
        if (d[i].InstanceContributionToHitGroupIndex > t.maxContribution)
            t.maxContribution = d[i].InstanceContributionToHitGroupIndex;
        auto it = g_blas.find(d[i].AccelerationStructure);
        if (it == g_blas.end()) { ++t.unknownBlas; continue; }
        switch (it->second.kind) {
            case Kind::kTriangles:  t.anyTriangles = true; break;
            case Kind::kProcedural: t.anyProcedural = true; break;
            case Kind::kMixed:      t.anyTriangles = t.anyProcedural = true; break;
            default:                ++t.unknownBlas; break;
        }
    }

    const bool isNew = g_tlas.find(tlas) == g_tlas.end();
    g_tlas[tlas] = t;
    if (!isNew) return;

    ++g_summary.tlasRead;
    if (t.maxContribution > g_summary.maxContribution)
        g_summary.maxContribution = t.maxContribution;

    if (g_summary.tlasRead <= 4)
        ProxyLog("[dxr11-proxy] top-level AS at 0x%llX READ: %u instances, max "
                 "InstanceContributionToHitGroupIndex %u, geometry reached: %s%s%s, "
                 "%u instance(s) pointing at an unseen bottom-level structure.\n",
                 static_cast<unsigned long long>(tlas), t.instanceCount,
                 t.maxContribution,
                 t.anyTriangles ? "triangles" : "",
                 (t.anyTriangles && t.anyProcedural) ? " and " : "",
                 t.anyProcedural ? "procedural" : "",
                 t.unknownBlas);

    // The two things the shader table cannot currently express. Said plainly,
    // once each, now that they are measured rather than assumed.
    static bool warnedContribution = false;
    if (t.maxContribution != 0 && !warnedContribution) {
        warnedContribution = true;
        ProxyLog("[dxr11-proxy] NOTE: this scene uses nonzero hit group "
                 "contributions (max %u). A lowered RayQuery dispatch builds "
                 "ONE hit group record and would resolve to the wrong one.\n",
                 t.maxContribution);
    }
    static bool warnedMixed = false;
    if (t.anyTriangles && t.anyProcedural && !warnedMixed) {
        warnedMixed = true;
        ProxyLog("[dxr11-proxy] NOTE: this scene reaches BOTH triangle and "
                 "procedural geometry from one top-level structure. A lowered "
                 "RayQuery dispatch builds records of one type only.\n");
    }
}

}  // namespace

const char* KindName(Kind k) {
    switch (k) {
        case Kind::kTriangles:  return "triangles";
        case Kind::kProcedural: return "procedural AABBs";
        case Kind::kMixed:      return "MIXED";
        default:                return "unknown";
    }
}

void NoteBuild(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc) {
    if (!desc) return;
    const auto& in = desc->Inputs;

    if (in.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
        // InstanceDescs is a GPU virtual address, and a copy needs a resource.
        // Ask the tracker to name it. Outside the lock: it takes its own.
        const restrack::Found src = restrack::Find(in.InstanceDescs);

        std::lock_guard<std::mutex> g(g_lock);
        ++g_summary.tlasCount;
        if (src.resource) ++g_summary.tlasResolved;
        if (!g_summary.sawTopLevel) {
            g_summary.sawTopLevel = true;
            if (src.resource) {
                ProxyLog("[dxr11-proxy] top-level AS build seen, %u instances. "
                         "Instance buffer at 0x%llX RESOLVED to resource %p + "
                         "0x%llX, out of %u tracked buffers, so the "
                         "descriptions can be read from it.\n",
                         in.NumDescs,
                         static_cast<unsigned long long>(in.InstanceDescs),
                         static_cast<void*>(src.resource),
                         static_cast<unsigned long long>(src.offset),
                         static_cast<unsigned>(restrack::Count()));
            } else {
                ProxyLog("[dxr11-proxy] top-level AS build seen, %u instances. "
                         "Instance buffer at 0x%llX did NOT resolve to any of %u "
                         "tracked buffers, so the contributions cannot be "
                         "copied; the table assumes zero.\n",
                         in.NumDescs,
                         static_cast<unsigned long long>(in.InstanceDescs),
                         static_cast<unsigned>(restrack::Count()));
            }
        }
        return;
    }
    if (in.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
        return;

    // ARRAY gives a plain CPU array; ARRAY_OF_POINTERS gives CPU pointers to
    // CPU descs. Both are readable. Anything else is not, and is left unknown
    // rather than guessed at.
    const bool ptrs = in.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
    if (in.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY && !ptrs) return;
    if (!ptrs && !in.pGeometryDescs) return;
    if (ptrs && !in.ppGeometryDescs) return;

    BlasInfo info;
    info.geometryCount = in.NumDescs;
    bool sawTri = false, sawProc = false;
    for (UINT i = 0; i < in.NumDescs; ++i) {
        const D3D12_RAYTRACING_GEOMETRY_DESC* g =
            ptrs ? in.ppGeometryDescs[i] : &in.pGeometryDescs[i];
        if (!g) continue;
        if (g->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS)
            sawProc = true;
        else
            sawTri = true;
    }
    info.kind = (sawTri && sawProc) ? Kind::kMixed
              : sawProc            ? Kind::kProcedural
              : sawTri             ? Kind::kTriangles
                                   : Kind::kUnknown;

    std::lock_guard<std::mutex> g(g_lock);
    const bool isNew = g_blas.find(desc->DestAccelerationStructureData) == g_blas.end();
    g_blas[desc->DestAccelerationStructureData] = info;
    if (!isNew) return;   // a rebuild of the same structure, already counted

    ++g_summary.blasCount;
    switch (info.kind) {
        case Kind::kTriangles:  ++g_summary.triangles; break;
        case Kind::kProcedural: ++g_summary.procedural; break;
        case Kind::kMixed:      ++g_summary.mixed; break;
        default: break;
    }
    // One line per distinct structure. An engine builds a great many, so this
    // is capped; the counts in the summary stay accurate regardless.
    if (g_summary.blasCount <= 8)
        ProxyLog("[dxr11-proxy] bottom-level AS at 0x%llX: %u geometr%s, %s\n",
                 static_cast<unsigned long long>(desc->DestAccelerationStructureData),
                 info.geometryCount, info.geometryCount == 1 ? "y" : "ies",
                 KindName(info.kind));
}

BlasInfo Lookup(D3D12_GPU_VIRTUAL_ADDRESS address) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_blas.find(address);
    return it == g_blas.end() ? BlasInfo() : it->second;
}

void NoteInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                   const D3D12_RAYTRACING_INSTANCE_DESC* descs, UINT count) {
    if (!tlas || !descs || !count) return;
    std::lock_guard<std::mutex> g(g_lock);
    ParseLocked(tlas, descs, count);
}

void NotePendingInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                          ID3D12Resource* readback, UINT count) {
    if (!tlas || !readback || !count) return;
    PendingRead pr;
    pr.tlas = tlas;
    pr.readback = readback;
    pr.count = count;
    std::lock_guard<std::mutex> g(g_lock);
    g_pending.push_back(pr);
}

void AfterSubmit(ID3D12CommandQueue* queue) {
    if (!queue) return;
    std::lock_guard<std::mutex> g(g_lock);
    if (g_pending.empty()) return;

    // Stamp everything recorded but not yet submitted. One signal covers them
    // all, because they all went out on or before this submission.
    bool anyUnstamped = false;
    for (const PendingRead& pr : g_pending)
        if (pr.fenceValue == 0) { anyUnstamped = true; break; }
    if (anyUnstamped) {
        if (!g_fence) {
            Microsoft::WRL::ComPtr<ID3D12Device> dev;
            if (SUCCEEDED(g_pending.front().readback->GetDevice(IID_PPV_ARGS(&dev))))
                dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        }
        if (g_fence && SUCCEEDED(queue->Signal(g_fence.Get(), g_fenceValue + 1))) {
            ++g_fenceValue;
            for (PendingRead& pr : g_pending)
                if (pr.fenceValue == 0) pr.fenceValue = g_fenceValue;
        }
    }

    // Parse whatever the GPU is already past. Nothing waits.
    if (!g_fence) return;
    const UINT64 done = g_fence->GetCompletedValue();
    std::vector<PendingRead> still;
    for (PendingRead& pr : g_pending) {
        if (pr.fenceValue == 0 || pr.fenceValue > done) { still.push_back(pr); continue; }
        const SIZE_T bytes =
            static_cast<SIZE_T>(pr.count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        void* p = nullptr;
        D3D12_RANGE readRange{ 0, bytes };
        if (SUCCEEDED(pr.readback->Map(0, &readRange, &p)) && p) {
            ParseLocked(pr.tlas,
                        static_cast<const D3D12_RAYTRACING_INSTANCE_DESC*>(p), pr.count);
            D3D12_RANGE noWrite{ 0, 0 };
            pr.readback->Unmap(0, &noWrite);
        } else {
            ProxyLog("[dxr11-proxy] top-level AS at 0x%llX: could not map the "
                     "instance readback buffer.\n",
                     static_cast<unsigned long long>(pr.tlas));
        }
        // Dropped either way; a failed map will not start succeeding.
    }
    g_pending.swap(still);
}

TlasInfo LookupTlas(D3D12_GPU_VIRTUAL_ADDRESS address) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_tlas.find(address);
    return it == g_tlas.end() ? TlasInfo() : it->second;
}

bool TableWouldBeWrong(std::string* why) {
    std::lock_guard<std::mutex> g(g_lock);
    for (const auto& kv : g_tlas) {
        const TlasInfo& t = kv.second;
        if (!t.valid) continue;
        if (t.maxContribution != 0) {
            if (why)
                *why = "the scene uses nonzero "
                       "InstanceContributionToHitGroupIndex, so a single hit "
                       "group record would not be the one the ray resolves to";
            return true;
        }
        if (t.anyTriangles && t.anyProcedural) {
            if (why)
                *why = "the scene reaches both triangle and procedural geometry "
                       "from one top-level structure, and a hit group record is "
                       "one or the other";
            return true;
        }
    }
    return false;
}

Summary GetSummary() {
    std::lock_guard<std::mutex> g(g_lock);
    return g_summary;
}

}  // namespace astrack
