// What a DESERIALIZED acceleration structure contains, asked of the driver.
//
// A structure made by CopyRaytracingAccelerationStructure(DESERIALIZE) is a
// driver-opaque blob the shim never saw built: which level it is, a
// bottom-level one's geometry count and kinds, a top-level one's instances.
// Until 0.61.0 an instance on one, or a dispatch tracing one, was refused by
// name. D3D12 has one documented way to ask: the same copy in mode
// VISUALIZATION_DECODE_FOR_TOOLS, which writes a header (level, count)
// followed by the geometry or instance descriptions. The spec calls it
// "intended for tools such as PIX only, though nothing stops any app from
// using it", and since its v1.19 no copy mode needs developer mode.
// Measured by tier11/decodeprobe.cpp, developer mode off: WARP and the GTX
// 1070 both decode a deserialized bottom-level structure's geometries (count
// and kind; flags differ) and a deserialized top-level one's instances
// exactly.
//
// Two steps, because the decode's size is only known from the GPU: right
// after the application's deserialize, in its own list, the shim records the
// size query (Note). Then, once that has run, at the split of a dispatch
// waiting for it, the shim records the decode into a list of its own on the
// same queue, submits it, waits once and hands the result to the tracker
// (Bring, astrack::ApplyDecoded). A dispatch meeting a structure that is
// still being decoded is deferred to its split rather than refused. The wait
// is paid once per deserialized structure, which an engine does at load.
#pragma once

#include <d3d12.h>

namespace asdecode {

// After the application's DESERIALIZE into `dst` is recorded into `cl`, the
// list `owner`, in its segment `segment` (the splits recorded before it).
void Note(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, D3D12_GPU_VIRTUAL_ADDRESS dst,
          const void* owner, UINT segment);

// Any deserialized structure not decoded yet.
bool Pending();

// Every submission: the size queries of the lists submitted are stamped with
// a fence on `queue`.
void AfterSubmit(ID3D12CommandQueue* queue, ID3D12CommandList* const* lists, UINT count);

// A list reset or destroyed without being submitted: its queries go.
void DropUnsubmitted(const void* owner);

typedef void (STDMETHODCALLTYPE* SubmitFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// At a split of `list`, its segments up to `segmentDone` run on `queue`:
// decodes every structure whose size query has run (in those segments, or
// stamped and past its fence), through `submit`, the queue's ORIGINAL
// ExecuteCommandLists, waiting once. Returns how many were applied.
int Bring(ID3D12CommandQueue* queue, SubmitFn submit, const void* list, UINT segmentDone);

}  // namespace asdecode
