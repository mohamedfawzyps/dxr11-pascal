#include "pso_stream.h"

#include <cstring>

namespace psostream {
namespace {

// Each subobject is a type tag followed by its payload, and the whole thing is
// aligned to a pointer. That is what the CD3DX12_PIPELINE_STATE_STREAM helpers
// produce with `alignas(void*)`, and it is what the runtime reads.
const SIZE_T kAlign = sizeof(void*);

SIZE_T AlignUp(SIZE_T n, SIZE_T a) { return (n + a - 1) & ~(a - 1); }

// A subobject is `alignas(void*) struct { TYPE tag; Inner payload; }`, which is
// what the CD3DX12 helpers emit and what the runtime reads. So the payload sits
// at the natural alignment of Inner, NOT at a fixed offset:
//
//   Inner = ID3D12RootSignature*   tag(4) pad(4) ptr(8)   -> payload at 8, 16 bytes
//   Inner = UINT                   tag(4) uint(4)         -> payload at 4,  8 bytes
//
// Getting that wrong desynchronises the walk on the first NODE_MASK and every
// byte after it is read as something it is not. Found by the first test that
// built a stream the way an engine builds one, which is the only reason this
// is right: reasoning about it produced the fixed offset.
//
// alignof(T) rather than a hand-kept table, so the compiler owns the answer.
struct Layout { SIZE_T size; SIZE_T align; };
#define LAY(T) Layout{ sizeof(T), alignof(T) }

// Layout for a type, or {0,0} for "not known here".
//
// Returning 0 STOPS the walk. That is deliberate and is the safe direction: a
// guessed size walks into whatever follows and reads it as a type tag, which
// could hand a shader pointer to something that is not one.
Layout PayloadLayout(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t) {
    switch (t) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            return LAY(ID3D12RootSignature*);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            return LAY(D3D12_SHADER_BYTECODE);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
            return LAY(D3D12_STREAM_OUTPUT_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
            return LAY(D3D12_BLEND_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            return LAY(UINT);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
            return LAY(D3D12_RASTERIZER_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            return LAY(D3D12_DEPTH_STENCIL_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
            return LAY(D3D12_INPUT_LAYOUT_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
            return LAY(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
            return LAY(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
            return LAY(D3D12_RT_FORMAT_ARRAY);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
            return LAY(DXGI_FORMAT);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
            return LAY(DXGI_SAMPLE_DESC);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            return LAY(UINT);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            return LAY(D3D12_CACHED_PIPELINE_STATE);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
            return LAY(D3D12_PIPELINE_STATE_FLAGS);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
            return LAY(D3D12_DEPTH_STENCIL_DESC1);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            return LAY(D3D12_VIEW_INSTANCING_DESC);
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2:
            return LAY(D3D12_DEPTH_STENCIL_DESC2);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1:
            return LAY(D3D12_RASTERIZER_DESC1);
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2:
            return LAY(D3D12_RASTERIZER_DESC2);
#endif
        default:
            return Layout{ 0, 0 };
    }
}

const char* ShaderKind(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t) {
    switch (t) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: return "vertex";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: return "pixel";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: return "domain";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: return "hull";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: return "geometry";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: return "compute";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: return "amplification";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return "mesh";
        default: return nullptr;
    }
}

}  // namespace

const char* TypeName(UINT type) {
    switch (static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(type)) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return "ROOT_SIGNATURE";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:  return "STREAM_OUTPUT";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:          return "BLEND";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:    return "SAMPLE_MASK";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:     return "RASTERIZER";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:  return "DEPTH_STENCIL";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:   return "INPUT_LAYOUT";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:     return "CACHED_PSO";
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:          return "FLAGS";
        default: {
            const char* k = ShaderKind(static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(type));
            return k ? k : "an unrecognised subobject type";
        }
    }
}

Parsed Walk(const D3D12_PIPELINE_STATE_STREAM_DESC* d) {
    Parsed out;
    if (!d || !d->pPipelineStateSubobjectStream || d->SizeInBytes == 0) return out;

    const BYTE* p = static_cast<const BYTE*>(d->pPipelineStateSubobjectStream);
    SIZE_T left = d->SizeInBytes;
    const SIZE_T tag = sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE);

    while (left >= kAlign) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        std::memcpy(&type, p, tag);

        const Layout lay = PayloadLayout(type);
        if (lay.size == 0) { out.stoppedAt = static_cast<UINT>(type); return out; }

        const SIZE_T at = AlignUp(tag, lay.align);
        const SIZE_T step = AlignUp(at + lay.size, kAlign);
        if (step > left) { out.stoppedAt = static_cast<UINT>(type); return out; }

        const BYTE* body = p + at;
        switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
                std::memcpy(&out.rootSignature, body, sizeof(out.rootSignature));
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
                std::memcpy(&out.nodeMask, body, sizeof(out.nodeMask));
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
                std::memcpy(&out.cachedPso, body, sizeof(out.cachedPso));
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
                std::memcpy(&out.flags, body, sizeof(out.flags));
                break;
            default:
                if (const char* kind = ShaderKind(type)) {
                    D3D12_SHADER_BYTECODE bc{};
                    std::memcpy(&bc, body, sizeof(bc));
                    if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) out.cs = bc;
                    else out.hasGraphicsStage = true;
                    if (out.shaderCount < 8 && bc.pShaderBytecode) {
                        out.shaders[out.shaderCount] = bc;
                        out.shaderKinds[out.shaderCount] = kind;
                        ++out.shaderCount;
                    }
                }
                break;
        }
        p += step;
        left -= step;
    }

    out.complete = true;
    return out;
}

}  // namespace psostream
