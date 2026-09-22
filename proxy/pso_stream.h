// Walking a D3D12_PIPELINE_STATE_STREAM_DESC.
//
// CreatePipelineState takes a packed stream of {type, payload} subobjects
// instead of a struct, and an engine with a unified PSO cache uses it for
// everything, compute included. Unreal does.
//
// The first version of this walker stopped at the first subobject that was not
// a shader, on the reasoning that its size was not knowable. That reasoning was
// wrong twice over: the size IS knowable, it is fixed per type, and a real
// stream puts ROOT_SIGNATURE first. So the walk ended before it reached the CS
// every single time.
//
// The cost of that was not a missing log line. RayQuery compute shaders reached
// the Tier 1.0 driver unmodified, the driver rejected them, and Unreal treats a
// failed compute PSO as fatal:
//
//     LowLevelFatalError [PipelineStateCache.cpp] Shader compilation failures are Fatal.
//
// A whole session, 8 acceleration structures and 208 state objects, and not one
// line in our log saying a RayQuery shader had arrived.
//
// So: every type's payload size is enumerated, and an UNKNOWN type still stops
// the walk, because guessing a size means walking into whatever follows. The
// difference is that stopping is now reported rather than silent.
#pragma once

#include <windows.h>
#include <d3d12.h>

namespace psostream {

struct Parsed {
    // The walk reached the end of the stream. False means it stopped early, and
    // `stoppedAt` says on what, so a stream shape we do not know about shows up
    // as a log line rather than as a shader that quietly slipped past.
    bool complete = false;
    UINT stoppedAt = 0;

    ID3D12RootSignature* rootSignature = nullptr;
    D3D12_SHADER_BYTECODE cs{};
    UINT nodeMask = 0;
    D3D12_CACHED_PIPELINE_STATE cachedPso{};
    D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // Any graphics or mesh stage at all. A stream carrying one is not a compute
    // pipeline whatever else it holds, and must never be substituted.
    bool hasGraphicsStage = false;

    // Every shader the stream carried, so RayQuery detection can report a
    // vertex or pixel shader too. Those have no lowering and the log is the
    // only place that will ever be said.
    D3D12_SHADER_BYTECODE shaders[8]{};
    const char* shaderKinds[8]{};
    int shaderCount = 0;

    bool IsComputeOnly() const {
        return !hasGraphicsStage && cs.pShaderBytecode != nullptr;
    }
};

Parsed Walk(const D3D12_PIPELINE_STATE_STREAM_DESC* d);

// A human name for a subobject type, for the log. Never null.
const char* TypeName(UINT type);

}  // namespace psostream
