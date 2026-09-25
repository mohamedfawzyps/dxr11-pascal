#include "res_tracker.h"

#include <map>
#include <mutex>

namespace restrack {
namespace {

struct Entry {
    UINT64 size = 0;
    ID3D12Resource* resource = nullptr;
    D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT;
};

std::mutex g_lock;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, Entry> g_buffers;   // keyed by start address

}  // namespace

void Note(ID3D12Resource* resource, bool reserved) {
    if (!resource) return;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    // Only buffers have a useful address range here, and only buffers are ever
    // looked up: instance descriptions live in one.
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    const D3D12_GPU_VIRTUAL_ADDRESS va = resource->GetGPUVirtualAddress();
    if (!va) return;   // an upload-only resource with no GPU address

    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
    // A reserved resource has no heap of its own, and asking is a debug layer
    // error (#901); its tiles live in whatever heaps it is mapped to, which
    // the CPU cannot read through it.
    const D3D12_HEAP_TYPE heap =
        !reserved && SUCCEEDED(resource->GetHeapProperties(&hp, &hf)) ? hp.Type
                                                                       : D3D12_HEAP_TYPE_DEFAULT;

    std::lock_guard<std::mutex> g(g_lock);
    g_buffers[va] = Entry{ desc.Width, resource, heap };   // replaces on address reuse
}

Found Find(D3D12_GPU_VIRTUAL_ADDRESS address) {
    if (!address) return Found();
    std::lock_guard<std::mutex> g(g_lock);
    // The last entry starting at or before the address is the only candidate.
    auto it = g_buffers.upper_bound(address);
    if (it == g_buffers.begin()) return Found();
    --it;
    if (address >= it->first + it->second.size) return Found();
    Found f;
    f.resource = it->second.resource;
    f.offset = address - it->first;
    f.heap = it->second.heap;
    return f;
}

size_t Count() {
    std::lock_guard<std::mutex> g(g_lock);
    return g_buffers.size();
}

}  // namespace restrack
