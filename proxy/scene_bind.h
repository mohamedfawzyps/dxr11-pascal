// Which descriptors hold a top-level acceleration structure, so a dispatch
// whose scene arrives through a descriptor table can be told WHICH scene it
// traces (GeometryIndex(), proxy/geom_index_so.h).
//
// A descriptor's contents are the driver's own format and cannot be read
// back, so every write the application makes through the wrapped device is
// followed instead: an SRV of RAYTRACING_ACCELERATION_STRUCTURE records its
// address, any other view written over it forgets it, and CopyDescriptors
// carries it along. Keyed by CPU descriptor handle, which is unique in the
// process; a new heap forgets whatever its range held before.
//
// Only CBV_SRV_UAV descriptors are followed. A write the shim cannot see (a
// device reached some other way than the one the shim handed out) is not
// followed; Lookup can then miss, never answer from a different heap.
#pragma once

#include <d3d12.h>

#include <utility>
#include <vector>

namespace scenebind {

void NoteHeap(ID3D12Device* real, ID3D12DescriptorHeap* heap);
void NoteSrv(D3D12_CPU_DESCRIPTOR_HANDLE at, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc);
void NoteOverwrite(D3D12_CPU_DESCRIPTOR_HANDLE at);
void NoteCopy(ID3D12Device* real, UINT numDst, const D3D12_CPU_DESCRIPTOR_HANDLE* dst,
              const UINT* dstSizes, UINT numSrc, const D3D12_CPU_DESCRIPTOR_HANDLE* src,
              const UINT* srcSizes, D3D12_DESCRIPTOR_HEAP_TYPE type);

// The structure the descriptor at `gpu` names, found through the bound heaps;
// 0 when it holds none the shim saw written.
D3D12_GPU_VIRTUAL_ADDRESS Lookup(ID3D12DescriptorHeap* const* heaps, UINT n,
                                 D3D12_GPU_DESCRIPTOR_HANDLE gpu);

// The same for ResourceDescriptorHeap[slot]: the bound shader-visible
// CBV_SRV_UAV heap's slot. `*inHeap` false when no such heap is bound or the
// slot is past its end.
D3D12_GPU_VIRTUAL_ADDRESS LookupSlot(ID3D12DescriptorHeap* const* heaps, UINT n, UINT slot,
                                     bool* inHeap);

// Every structure written to the bound shader-visible CBV_SRV_UAV heap from
// the descriptor at `from` on, at most `count` of them (to the heap's end):
// (index from `from`, address), in index order. A scene picked at run time
// from an unbounded array or the heap itself is one of these (0.60.0).
// `from` 0 is the heap's start. `*inHeap` false when no such heap is bound
// or `from` is not in it.
std::vector<std::pair<UINT, D3D12_GPU_VIRTUAL_ADDRESS>> Enumerate(
    ID3D12DescriptorHeap* const* heaps, UINT n, D3D12_GPU_DESCRIPTOR_HANDLE from, UINT64 count,
    bool* inHeap);

}  // namespace scenebind
