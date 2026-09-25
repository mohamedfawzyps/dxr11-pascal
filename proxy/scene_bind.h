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

}  // namespace scenebind
