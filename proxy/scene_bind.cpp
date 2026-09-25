// See the header.
#include "scene_bind.h"

#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <utility>
#include <vector>

namespace scenebind {
namespace {

// CPU descriptor handles are NOT addresses on every driver. Measured on the
// GTX 1070: small encoded numbers, INTERLEAVED between heaps (one heap's
// slots 0x1, 0x21, 0x41, another's 0x2, 0x22), so a byte range of one heap
// contains another heap's descriptors. So nothing here works on ranges: only
// on the exact handles start + i * increment. A handle is unique in the
// process, which is all the driver itself can rely on.
std::mutex g_lock;
std::unordered_map<SIZE_T, D3D12_GPU_VIRTUAL_ADDRESS> g_tlas;  // CPU descriptor -> structure
std::atomic<bool> g_any{ false };                              // anything ever recorded

// Visits every recorded handle among start + i * inc, i < n, whichever of the
// two is smaller to walk: the n handles, or the recorded ones.
template <class F>
void EachIn(SIZE_T start, SIZE_T n, SIZE_T inc, F&& f) {
    if (!inc) return;
    if (n <= g_tlas.size()) {
        for (SIZE_T i = 0; i < n; ++i) {
            auto it = g_tlas.find(start + i * inc);
            if (it != g_tlas.end()) f(i, it->second);
        }
        return;
    }
    for (const auto& kv : g_tlas)
        if (kv.first >= start && (kv.first - start) % inc == 0 && (kv.first - start) / inc < n)
            f((kv.first - start) / inc, kv.second);
}

void EraseLocked(SIZE_T start, SIZE_T n, SIZE_T inc) {
    std::vector<SIZE_T> gone;
    EachIn(start, n, inc, [&](SIZE_T i, D3D12_GPU_VIRTUAL_ADDRESS) { gone.push_back(start + i * inc); });
    for (auto k : gone) g_tlas.erase(k);
}

UINT Increment(ID3D12Device* real) {
    return real->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

}  // namespace

void NoteHeap(ID3D12Device* real, ID3D12DescriptorHeap* heap) {
    if (!heap || !g_any.load(std::memory_order_relaxed)) return;
    const D3D12_DESCRIPTOR_HEAP_DESC d = heap->GetDesc();
    if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return;
    const SIZE_T start = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    const SIZE_T inc = Increment(real);
    std::lock_guard<std::mutex> g(g_lock);
    EraseLocked(start, d.NumDescriptors, inc);
}

void NoteSrv(D3D12_CPU_DESCRIPTOR_HANDLE at, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc) {
    if (desc && desc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE) {
        std::lock_guard<std::mutex> g(g_lock);
        g_tlas[at.ptr] = desc->RaytracingAccelerationStructure.Location;
        g_any.store(true, std::memory_order_relaxed);
        return;
    }
    NoteOverwrite(at);
}

void NoteOverwrite(D3D12_CPU_DESCRIPTOR_HANDLE at) {
    if (!g_any.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> g(g_lock);
    g_tlas.erase(at.ptr);
}

void NoteCopy(ID3D12Device* real, UINT numDst, const D3D12_CPU_DESCRIPTOR_HANDLE* dst,
              const UINT* dstSizes, UINT numSrc, const D3D12_CPU_DESCRIPTOR_HANDLE* src,
              const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !g_any.load(std::memory_order_relaxed))
        return;
    const SIZE_T inc = Increment(real);
    // Pair the two range lists descriptor by descriptor, as D3D12 does, into
    // runs of (src, dst, count). Collected before anything is erased, so an
    // overlapping copy reads what was there.
    struct Run { SIZE_T s, d, n; };
    std::vector<Run> runs;
    UINT di = 0, si = 0, doff = 0, soff = 0;
    while (di < numDst && si < numSrc) {
        const UINT dn = dstSizes ? dstSizes[di] : 1, sn = srcSizes ? srcSizes[si] : 1;
        if (doff >= dn) { ++di; doff = 0; continue; }
        if (soff >= sn) { ++si; soff = 0; continue; }
        const UINT n = (std::min)(dn - doff, sn - soff);
        runs.push_back({ src[si].ptr + soff * inc, dst[di].ptr + doff * inc, n });
        doff += n; soff += n;
    }
    std::lock_guard<std::mutex> g(g_lock);
    std::vector<std::pair<SIZE_T, D3D12_GPU_VIRTUAL_ADDRESS>> carried;
    for (const auto& r : runs)
        EachIn(r.s, r.n, inc, [&](SIZE_T i, D3D12_GPU_VIRTUAL_ADDRESS a) {
            carried.emplace_back(r.d + i * inc, a);
        });
    for (const auto& r : runs) EraseLocked(r.d, r.n, inc);
    for (const auto& c : carried) g_tlas[c.first] = c.second;
}

D3D12_GPU_VIRTUAL_ADDRESS Lookup(ID3D12DescriptorHeap* const* heaps, UINT n,
                                 D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    for (UINT i = 0; i < n; ++i) {
        ID3D12DescriptorHeap* h = heaps[i];
        if (!h) continue;
        const D3D12_DESCRIPTOR_HEAP_DESC d = h->GetDesc();
        if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
            !(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
            continue;
        ID3D12Device* dev = nullptr;
        if (FAILED(h->GetDevice(IID_PPV_ARGS(&dev))) || !dev) continue;
        const UINT64 inc = Increment(dev);
        dev->Release();
        const UINT64 g0 = h->GetGPUDescriptorHandleForHeapStart().ptr;
        if (!inc || gpu.ptr < g0 || gpu.ptr >= g0 + d.NumDescriptors * inc || (gpu.ptr - g0) % inc)
            continue;
        // The same slot's CPU handle: the heap's CPU start plus the slot
        // times the increment, whatever the CPU handles encode.
        const SIZE_T cpu = h->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)((gpu.ptr - g0) / inc * inc);
        std::lock_guard<std::mutex> g(g_lock);
        auto it = g_tlas.find(cpu);
        return it == g_tlas.end() ? 0 : it->second;
    }
    return 0;
}

D3D12_GPU_VIRTUAL_ADDRESS LookupSlot(ID3D12DescriptorHeap* const* heaps, UINT n, UINT slot,
                                     bool* inHeap) {
    *inHeap = false;
    for (UINT i = 0; i < n; ++i) {
        ID3D12DescriptorHeap* h = heaps[i];
        if (!h) continue;
        const D3D12_DESCRIPTOR_HEAP_DESC d = h->GetDesc();
        if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
            !(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
            continue;
        if (slot >= d.NumDescriptors) return 0;
        ID3D12Device* dev = nullptr;
        if (FAILED(h->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return 0;
        const UINT64 inc = Increment(dev);
        dev->Release();
        *inHeap = true;
        D3D12_GPU_DESCRIPTOR_HANDLE g{ h->GetGPUDescriptorHandleForHeapStart().ptr + slot * inc };
        return Lookup(&h, 1, g);
    }
    return 0;
}

}  // namespace scenebind
