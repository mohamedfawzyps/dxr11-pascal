// Descriptors of the shim's own at the END of an application's
// shader-visible CBV/SRV/UAV heaps (0.63.0).
//
// Why: a local root signature past a size REMOVES THE DEVICE on a GTX 1070
// (tier11/lrsprobe.cpp), and the shim's own layouts give a shader one root
// SRV (2 dwords) per scene copy it can trace. Past that size the copies go in
// ONE descriptor table (1 dword, whatever it holds), and a table's
// descriptors have to be in the heap bound at the dispatch, which is the
// application's. So each such heap is created that much larger, and GetDesc
// is hooked to report the size the application asked for; nothing of the
// application's reaches the tail, and only the shim writes there.
//
// Measured (tier11/heapprobe.cpp): descriptor heaps share ONE vtable, in
// D3D12Core.dll (D3D12SDKLayers.dll with the debug layer), and slot 8 is
// GetDesc with the (this, RetVal*) ABI. The GTX 1070 creates a shader-visible
// heap of at most 1,000,000 descriptors, the Tier 1 and 2 maximum, and WARP 2
// million; so a heap is only grown to at most 1,000,000, which every device
// takes, and one already that large (Unreal's bindless heap) has no reserve.
// Unreal's ray tracing heap is 250,000 by default.
#pragma once

#include <d3d12.h>

#include <string>
#include <vector>

namespace heapres {

// For Dxr11Device::CreateDescriptorHeap, on the real device: the heap,
// grown by the reserve where it is a shader-visible CBV/SRV/UAV heap.
HRESULT Create(ID3D12Device* real, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out);

// One descriptor: a top-level structure by address, or a raw buffer SRV of
// a whole resource (the shim's pair and key tables).
struct View {
    D3D12_GPU_VIRTUAL_ADDRESS as = 0;
    ID3D12Resource* raw = nullptr;   // the shim holds it
    UINT64 bytes = 0;
    bool operator<(const View& o) const {
        if (as != o.as) return as < o.as;
        if (raw != o.raw) return raw < o.raw;
        return bytes < o.bytes;
    }
    bool operator==(const View& o) const { return as == o.as && raw == o.raw && bytes == o.bytes; }
};

// A range holding `views`, in order, in the reserve of the CBV/SRV/UAV heap
// among `heaps` (the heaps bound at the dispatch), held for `owner` until
// its list can no longer run. With no such heap bound, in the shim's own
// heap: `bind` then receives the heaps to bind for the dispatch (the
// application's with it added), and stays empty otherwise. A range with the
// same views is reused. False with *why set when the bound heap has no
// reserve or the reserve is full of ranges still in flight.
bool Range(ID3D12Device* real, const std::vector<ID3D12DescriptorHeap*>& heaps,
           const std::vector<View>& views, const void* owner, D3D12_GPU_DESCRIPTOR_HANDLE* out,
           std::vector<ID3D12DescriptorHeap*>* bind, std::string* why);

// The smallest form the shim's own layouts are built in: 0, unless
// DXR_TIER11_LRS_TABLE is 1 (the scene copies in a table even where root
// descriptors fit) or 2 (every addition in the table): the check that each
// form draws every case root descriptors do.
unsigned Forced();

}  // namespace heapres
