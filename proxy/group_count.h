// Indirect COMPUTE dispatch of a lowered RayQuery pipeline.
//
// Unreal dispatches many of its inline ray tracing passes with
// DispatchIndirect, which in D3D12 is ExecuteIndirect with a signature holding
// one DISPATCH argument (WindowsD3D12Device.cpp: NumArgumentDescs 1,
// ByteStride sizeof(D3D12_DISPATCH_ARGUMENTS), no root signature). With a
// lowered pipeline bound, the GPU would run the carrier, which does nothing.
// The dispatch has to become DispatchRays with groups times numthreads rays,
// and the group counts only exist on the GPU.
//
// So the shim reads them back, like the indirect DispatchRays path does, with
// one difference that matters: it never touches the application's argument
// buffer. That buffer is in whatever state the application's own barriers put
// it in, often INDIRECT_ARGUMENT combined with other read states, and a
// transition with a guessed StateBefore corrupts the application's tracking.
// Instead the shim replays the application's own ExecuteIndirect, same
// signature, same buffer, same offset, with a tiny compute shader of its own
// bound that records SV_GroupID + 1 with an atomic max. ExecuteIndirect needs
// the buffer in INDIRECT_ARGUMENT, and the application had already put it
// there, because it was about to do exactly this call. Everything the capture
// writes is the shim's own.

#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace groupcount {

// What a command signature can do for a lowered pipeline.
enum class Shape {
    kUnknown,        // not seen being created, or not a dispatch signature
    kDispatch,       // exactly one DISPATCH argument, no root signature: emulated
    kUnsupported,    // DISPATCH alongside other arguments, or a root signature
};

// Called for every command signature the application creates. Tags the real
// object with its shape and stride, as private data, so the tag lives and dies
// with the object and a reused pointer can never carry a stale answer.
void TagSignature(ID3D12CommandSignature* sig, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                  ID3D12RootSignature* rootSig);
Shape SignatureShape(ID3D12CommandSignature* sig, UINT* stride);

// The two buffers one capture writes. Both must live until the list holding
// the capture has executed.
struct Capture {
    Microsoft::WRL::ComPtr<ID3D12Resource> counts;     // default heap, UAV
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;   // readback heap, 12 bytes
};

// Records the capture of one command's group counts into `cl`. Replaces the
// compute root signature and the pipeline state; the CALLER restores them.
bool Record(ID3D12GraphicsCommandList4* cl, ID3D12Device* dev,
            ID3D12CommandSignature* sig, ID3D12Resource* args, UINT64 offset,
            Capture* out, std::string* why);

// After the capture has executed: the group counts. False if they cannot be
// read. Zeros mean a zero-sized dispatch, where no group ran.
bool Read(const Capture& c, UINT counts[3]);

}  // namespace groupcount
