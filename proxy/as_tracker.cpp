#include "as_tracker.h"

#include "proxy_log.h"

#include <map>
#include <mutex>

namespace astrack {
namespace {

std::mutex g_lock;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, BlasInfo> g_blas;
Summary g_summary;

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
        // InstanceDescs is a GPU virtual address, so the contributions cannot
        // be read here. Only the fact that a top-level build happened is
        // recorded, which is enough to say honestly that the other half is
        // missing.
        std::lock_guard<std::mutex> g(g_lock);
        ++g_summary.tlasCount;
        if (!g_summary.sawTopLevel) {
            g_summary.sawTopLevel = true;
            ProxyLog("[dxr11-proxy] top-level AS build seen, %u instances. Their "
                     "InstanceContributionToHitGroupIndex values live in GPU "
                     "memory and are NOT read; the shader table still assumes "
                     "they are all zero.\n", in.NumDescs);
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

Summary GetSummary() {
    std::lock_guard<std::mutex> g(g_lock);
    return g_summary;
}

}  // namespace astrack
