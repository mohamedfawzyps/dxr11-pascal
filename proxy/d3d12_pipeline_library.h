// A wrapper for ID3D12PipelineLibrary, for one reason: StorePipeline.
//
// Dxr11RayQueryPso is an ID3D12PipelineState the application holds and never
// inspects, and it is not a real D3D12 object. Every call that can carry one
// to the driver has to trap it. SetPipelineState always did; Reset, ClearState
// and CreateCommandList were found doing it in 0.31.0.
//
// StorePipeline is the last one, and the least obvious, because it does not
// look like a command list call at all. An engine with a PSO cache asks the
// runtime to SERIALIZE a pipeline state so it can be reloaded next launch.
// Unreal has exactly that, in PipelineStateCache.cpp, which is the file its
// fatal error named. Handing that machinery an object D3D12 did not create
// means it reads a vtable and fields that are not there.
//
// The library itself is forwarded untouched. Nothing here caches, rewrites or
// inspects anything; it only declines to store a stand-in.
#pragma once

#include <d3d12.h>

// Wrap a library the application just created. Returns S_OK and replaces
// *ppv with the wrapper, or leaves it alone if the riid is not a pipeline
// library, in which case the real object is passed through as before.
void Dxr11WrapPipelineLibrary(REFIID riid, void** ppv);
