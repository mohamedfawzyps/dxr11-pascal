#include "group_count.h"
#include "group_count_cs.h"
#include "instance_copy_cs.h"

#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace groupcount {
namespace {

// {5C1E7A93-0B4D-4E2F-A8C6-3D9F17B2E640}
const GUID kSignatureTag =
    { 0x5c1e7a93, 0x0b4d, 0x4e2f, { 0xa8, 0xc6, 0x3d, 0x9f, 0x17, 0xb2, 0xe6, 0x40 } };

struct Tag {
    UINT shape;
    UINT stride;
};

// The capture pipeline, one per device. The root signature is embedded in the
// shader, so nothing needs serialising at run time.
struct Pipeline {
    ID3D12Device* device = nullptr;   // compared, not owned
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
};
std::mutex g_lock;
Pipeline g_pipe, g_copyPipe;

bool GetPipeline(ID3D12Device* dev, Pipeline* slot, const void* cs, size_t size,
                 Pipeline* out, std::string* why) {
    std::lock_guard<std::mutex> g(g_lock);
    if (slot->device != dev || !slot->pso) {
        Pipeline p;
        p.device = dev;
        HRESULT hr = dev->CreateRootSignature(0, cs, size, IID_PPV_ARGS(&p.rs));
        if (FAILED(hr)) {
            if (why) *why = "could not create a capture root signature";
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = p.rs.Get();
        pd.CS.pShaderBytecode = cs;
        pd.CS.BytecodeLength = size;
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&p.pso));
        if (FAILED(hr)) {
            if (why) *why = "could not create a capture pipeline";
            return false;
        }
        *slot = p;
    }
    *out = *slot;
    return true;
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device* dev, D3D12_HEAP_TYPE type,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                              UINT64 width = 3 * sizeof(UINT)) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = width;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    // A committed resource is zeroed on creation, which the atomic max relies
    // on: a dispatch with no groups must read back as zeros.
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                            nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

}  // namespace

void TagSignature(ID3D12CommandSignature* sig, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                  ID3D12RootSignature* rootSig) {
    if (!sig || !desc) return;
    bool dispatch = false;
    for (UINT i = 0; i < desc->NumArgumentDescs; ++i)
        if (desc->pArgumentDescs[i].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH)
            dispatch = true;
    if (!dispatch) return;
    Tag t;
    t.shape = (desc->NumArgumentDescs == 1 && !rootSig)
                  ? static_cast<UINT>(Shape::kDispatch)
                  : static_cast<UINT>(Shape::kUnsupported);
    t.stride = desc->ByteStride;
    sig->SetPrivateData(kSignatureTag, sizeof(t), &t);
}

Shape SignatureShape(ID3D12CommandSignature* sig, UINT* stride) {
    if (!sig) return Shape::kUnknown;
    Tag t{};
    UINT size = sizeof(t);
    if (FAILED(sig->GetPrivateData(kSignatureTag, &size, &t)) || size != sizeof(t))
        return Shape::kUnknown;
    if (stride) *stride = t.stride;
    return static_cast<Shape>(t.shape);
}

bool Record(ID3D12GraphicsCommandList4* cl, ID3D12Device* dev,
            ID3D12CommandSignature* sig, ID3D12Resource* args, UINT64 offset,
            Capture* out, std::string* why) {
    if (!cl || !dev || !sig || !args || !out) return false;
    Pipeline p;
    if (!GetPipeline(dev, &g_pipe, g_groupCountCS, sizeof(g_groupCountCS), &p, why))
        return false;

    Capture c;
    c.counts = Buffer(dev, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COMMON);
    c.readback = Buffer(dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST);
    if (!c.counts || !c.readback) {
        if (why) *why = "could not create the group count buffers";
        return false;
    }

    // Only the shim's own buffer changes state. The application's argument
    // buffer is used exactly as the application was about to use it.
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = c.counts.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);

    cl->SetComputeRootSignature(p.rs.Get());
    cl->SetPipelineState(p.pso.Get());
    cl->SetComputeRootUnorderedAccessView(0, c.counts->GetGPUVirtualAddress());
    cl->ExecuteIndirect(sig, 1, args, offset, nullptr, 0);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    cl->CopyBufferRegion(c.readback.Get(), 0, c.counts.Get(), 0, 3 * sizeof(UINT));

    *out = c;
    return true;
}

bool RecordRawCopy(ID3D12GraphicsCommandList4* cl, ID3D12Device* dev,
                   D3D12_GPU_VIRTUAL_ADDRESS src, UINT dwords,
                   Capture* out, std::string* why) {
    if (!cl || !dev || !src || !dwords || !out) return false;
    const UINT groups = (dwords + 63) / 64;
    if (groups > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) {
        if (why) *why = "too many instance descriptions to copy in one dispatch";
        return false;
    }
    Pipeline p;
    if (!GetPipeline(dev, &g_copyPipe, g_instanceCopyCS, sizeof(g_instanceCopyCS), &p, why))
        return false;

    const UINT64 bytes = static_cast<UINT64>(dwords) * sizeof(UINT);
    Capture c;
    c.counts = Buffer(dev, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COMMON, bytes);
    c.readback = Buffer(dev, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, bytes);
    if (!c.counts || !c.readback) {
        if (why) *why = "could not create the instance copy buffers";
        return false;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = c.counts.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);

    cl->SetComputeRootSignature(p.rs.Get());
    cl->SetPipelineState(p.pso.Get());
    cl->SetComputeRoot32BitConstant(0, dwords, 0);
    cl->SetComputeRootShaderResourceView(1, src);
    cl->SetComputeRootUnorderedAccessView(2, c.counts->GetGPUVirtualAddress());
    cl->Dispatch(groups, 1, 1);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    cl->CopyBufferRegion(c.readback.Get(), 0, c.counts.Get(), 0, bytes);

    *out = c;
    return true;
}

bool Read(const Capture& c, UINT counts[3]) {
    if (!c.readback) return false;
    void* p = nullptr;
    D3D12_RANGE all{ 0, 3 * sizeof(UINT) };
    if (FAILED(c.readback->Map(0, &all, &p)) || !p) return false;
    std::memcpy(counts, p, 3 * sizeof(UINT));
    D3D12_RANGE noWrite{ 0, 0 };
    c.readback->Unmap(0, &noWrite);
    return true;
}

}  // namespace groupcount
