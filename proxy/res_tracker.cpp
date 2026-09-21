#include "res_tracker.h"

#include <map>
#include <mutex>

namespace restrack {
namespace {

struct Entry {
    UINT64 size = 0;
    ID3D12Resource* resource = nullptr;
};

std::mutex g_lock;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, Entry> g_buffers;   // keyed by start address

}  // namespace

void Note(ID3D12Resource* resource) {
    if (!resource) return;
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    // Only buffers have a useful address range here, and only buffers are ever
    // looked up: instance descriptions live in one.
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    const D3D12_GPU_VIRTUAL_ADDRESS va = resource->GetGPUVirtualAddress();
    if (!va) return;   // an upload-only resource with no GPU address

    std::lock_guard<std::mutex> g(g_lock);
    g_buffers[va] = Entry{ desc.Width, resource };   // replaces on address reuse
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
    return f;
}

size_t Count() {
    std::lock_guard<std::mutex> g(g_lock);
    return g_buffers.size();
}

}  // namespace restrack
