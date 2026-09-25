// Turning a GPU virtual address back into the resource that owns it.
//
// The TLAS half of the acceleration structure problem needs to READ the
// instance descriptions, and they arrive as a GPU virtual address. Copying
// from GPU memory needs `CopyBufferRegion`, which takes an ID3D12Resource and
// an offset, not an address, and **D3D12 has no API to go from an address back
// to a resource**. So the shim has to remember, which it can, because every
// resource the application creates goes through the wrapped device.
//
// Two deliberate decisions, both about lifetime.
//
// NO REFERENCE IS HELD. AddRef here would change when the application's
// resources die, which a transparent shim must not do, and would leak for
// every resource an application ever creates. Entries can therefore go stale.
// That is safe for the one use there is: a lookup only happens while the
// application is passing that buffer to a build, so it must still own it.
//
// AN ENTRY IS REPLACED when a new resource reports the same start address,
// which is how address reuse after a free is handled. Partial overlap between
// a stale entry and a live one is possible in principle with placed resources;
// the copy would then fail visibly rather than silently read the wrong thing.
#pragma once

#include <d3d12.h>

namespace restrack {

// Remember a freshly created buffer. Anything that is not a buffer, or has no
// virtual address, is ignored.
void Note(ID3D12Resource* resource, bool reserved = false);

struct Found {
    ID3D12Resource* resource = nullptr;
    UINT64 offset = 0;          // of `address` within the resource
    // Recorded at creation, so asking never calls into a resource that may
    // since have been freed: the tracker holds no reference.
    D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT;
};

// The resource containing this address, or an empty Found.
Found Find(D3D12_GPU_VIRTUAL_ADDRESS address);

// How many buffers are being tracked. For the log and for tests.
size_t Count();

}  // namespace restrack
