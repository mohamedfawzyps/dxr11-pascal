#include "rq_pipeline.h"

#include "proxy_log.h"
#include "rewriter/dxc_host.h"
#include "rewriter/ll_model.h"
#include "rewriter/rq_analyze.h"
#include "rewriter/rq_lower.h"

#include <cstring>
#include <new>

const GUID IID_Dxr11RayQueryPso =
    { 0x7e3b1c42, 0x9a54, 0x4d18, { 0x8f, 0x60, 0x2c, 0x71, 0xb0, 0xa4, 0xe9, 0xd3 } };

namespace {

const UINT kIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;                  // 32
const UINT kRecAlign = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;        // 32
const UINT kTableAlign = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;       // 64
const UINT kPayloadBytes = 88;   // must match PAYLOAD_BYTES in rq_lower.cpp
const UINT kAttrBytes = 8;

UINT AlignUp(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }

// Context handed through the DXC host's transform callback.
struct Xform {
    std::string why;
    bool hasAnyHit = false;
    int threads[3] = { 1, 1, 1 };
};

bool DoLower(const std::string& in, std::string* out, std::string* why, void* ctx) {
    Xform* x = static_cast<Xform*>(ctx);
    llm::Module m(llm::Normalize(in));
    auto a = rq::Analyze(m);
    if (!a.ok) { *why = a.error; return false; }
    if (!rq::NumThreads(m, x->threads)) {
        *why = "the compute shader declares no numthreads, so the ray grid "
               "cannot be worked out";
        return false;
    }
    auto l = rq::Lower(m, a.query);
    if (!l.ok) { *why = l.error; return false; }
    x->hasAnyHit = a.query.NeedsAnyHit();
    *out = l.text;
    return true;
}

}  // namespace

Dxr11RayQueryPso::~Dxr11RayQueryPso() {
    if (m_sbt) m_sbt->Release();
    if (m_so) m_so->Release();
    if (m_rootSig) m_rootSig->Release();
}

Dxr11RayQueryPso* Dxr11RayQueryPso::From(ID3D12PipelineState* p) {
    if (!p) return nullptr;
    Dxr11RayQueryPso* self = nullptr;
    if (SUCCEEDED(p->QueryInterface(IID_Dxr11RayQueryPso, reinterpret_cast<void**>(&self))))
        return self;   // QueryInterface does not AddRef for the private IID
    return nullptr;
}

Dxr11RayQueryPso* Dxr11RayQueryPso::TryCreate(
        ID3D12Device5* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
        std::string* why) {
    if (!dev || !desc || !desc->CS.pShaderBytecode) {
        *why = "no compute shader bytecode";
        return nullptr;
    }

    // --- rewrite ------------------------------------------------------------
    Xform x;
    std::vector<uint8_t> lib;
    std::string err;
    if (!dxch::RewriteContainer(desc->CS.pShaderBytecode,
                                static_cast<size_t>(desc->CS.BytecodeLength),
                                DoLower, &x, &lib, &err)) {
        *why = err;
        return nullptr;
    }

    // --- state object -------------------------------------------------------
    // The application's compute root signature becomes the GLOBAL root
    // signature, which is what makes its existing bindings work unchanged.
    D3D12_DXIL_LIBRARY_DESC libDesc{};
    libDesc.DXILLibrary.pShaderBytecode = lib.data();
    libDesc.DXILLibrary.BytecodeLength = lib.size();

    D3D12_HIT_GROUP_DESC hg{};
    hg.HitGroupExport = L"HitGroup";
    hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg.ClosestHitShaderImport = L"ClosestHit";
    if (x.hasAnyHit) hg.AnyHitShaderImport = L"AnyHit";

    D3D12_RAYTRACING_SHADER_CONFIG sc{};
    sc.MaxPayloadSizeInBytes = kPayloadBytes;
    sc.MaxAttributeSizeInBytes = kAttrBytes;

    // Inline queries do not recurse, so one level is all a lowered shader can
    // ever need. The brief says as much.
    D3D12_RAYTRACING_PIPELINE_CONFIG pc{};
    pc.MaxTraceRecursionDepth = 1;

    D3D12_GLOBAL_ROOT_SIGNATURE grs{};
    grs.pGlobalRootSignature = desc->pRootSignature;

    D3D12_STATE_SUBOBJECT subs[5]{};
    UINT n = 0;
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc };
    if (grs.pGlobalRootSignature)
        subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs };

    D3D12_STATE_OBJECT_DESC sod{};
    sod.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    sod.NumSubobjects = n;
    sod.pSubobjects = subs;

    ID3D12StateObject* so = nullptr;
    HRESULT hr = dev->CreateStateObject(&sod, IID_PPV_ARGS(&so));
    if (FAILED(hr)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "CreateStateObject failed (hr=0x%08X)",
                      static_cast<unsigned>(hr));
        *why = buf;
        return nullptr;
    }

    ID3D12StateObjectProperties* props = nullptr;
    if (FAILED(so->QueryInterface(IID_PPV_ARGS(&props)))) {
        so->Release();
        *why = "no ID3D12StateObjectProperties on the state object";
        return nullptr;
    }

    // --- shader table -------------------------------------------------------
    // One record each, raygen then miss then hit group, each table aligned.
    const UINT stride = AlignUp(kIdSize, kRecAlign);
    const UINT slot = AlignUp(stride, kTableAlign);
    const UINT64 size = static_cast<UINT64>(slot) * 3;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* sbt = nullptr;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&sbt));
    if (FAILED(hr)) {
        props->Release(); so->Release();
        *why = "cannot create the shader table buffer";
        return nullptr;
    }

    uint8_t* p = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(sbt->Map(0, &none, reinterpret_cast<void**>(&p)))) {
        sbt->Release(); props->Release(); so->Release();
        *why = "cannot map the shader table";
        return nullptr;
    }
    std::memset(p, 0, static_cast<size_t>(size));
    const void* idRay = props->GetShaderIdentifier(L"RayGen");
    const void* idMiss = props->GetShaderIdentifier(L"Miss");
    const void* idHit = props->GetShaderIdentifier(L"HitGroup");
    if (!idRay || !idMiss || !idHit) {
        sbt->Unmap(0, nullptr); sbt->Release(); props->Release(); so->Release();
        *why = "the state object does not export RayGen, Miss and HitGroup";
        return nullptr;
    }
    std::memcpy(p + 0 * slot, idRay, kIdSize);
    std::memcpy(p + 1 * slot, idMiss, kIdSize);
    std::memcpy(p + 2 * slot, idHit, kIdSize);
    sbt->Unmap(0, nullptr);
    props->Release();

    auto* self = new (std::nothrow) Dxr11RayQueryPso();
    if (!self) {
        sbt->Release(); so->Release();
        *why = "out of memory";
        return nullptr;
    }
    self->m_dev = dev;
    self->m_so = so;
    self->m_sbt = sbt;
    self->m_rootSig = desc->pRootSignature;
    if (self->m_rootSig) self->m_rootSig->AddRef();
    for (int i = 0; i < 3; ++i) self->m_threads[i] = static_cast<UINT>(x.threads[i]);

    const D3D12_GPU_VIRTUAL_ADDRESS base = sbt->GetGPUVirtualAddress();
    self->m_desc.RayGenerationShaderRecord.StartAddress = base + 0 * slot;
    self->m_desc.RayGenerationShaderRecord.SizeInBytes = kIdSize;
    self->m_desc.MissShaderTable.StartAddress = base + 1 * slot;
    self->m_desc.MissShaderTable.SizeInBytes = kIdSize;
    self->m_desc.MissShaderTable.StrideInBytes = stride;
    self->m_desc.HitGroupTable.StartAddress = base + 2 * slot;
    self->m_desc.HitGroupTable.SizeInBytes = kIdSize;
    self->m_desc.HitGroupTable.StrideInBytes = stride;

    ProxyLog("[dxr11-proxy] RayQuery compute shader lowered and ready: "
             "%zu -> %zu bytes, numthreads(%u,%u,%u)%s\n",
             static_cast<size_t>(desc->CS.BytecodeLength), lib.size(),
             self->m_threads[0], self->m_threads[1], self->m_threads[2],
             x.hasAnyHit ? ", with a generated any-hit shader" : "");
    return self;
}

void Dxr11RayQueryPso::DispatchAsRays(ID3D12GraphicsCommandList4* cl,
                                      UINT gx, UINT gy, UINT gz) {
    if (!cl) return;
    D3D12_DISPATCH_RAYS_DESC d = m_desc;
    // Dispatch counts thread GROUPS; DispatchRays counts RAYS. The lowered
    // raygen reads DispatchRaysIndex where the original read
    // SV_DispatchThreadID, which is the global thread id, so the ray grid is
    // the group count times the group size. The shader's own bounds check
    // discards the overhang, exactly as it did for threads.
    d.Width = gx * m_threads[0];
    d.Height = gy * m_threads[1];
    d.Depth = gz * m_threads[2];
    cl->SetPipelineState1(m_so);
    cl->DispatchRays(&d);
}

// --- IUnknown ---------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::QueryInterface(REFIID riid, void** pp) {
    if (!pp) return E_POINTER;
    if (riid == IID_Dxr11RayQueryPso) {
        // Deliberately no AddRef: this is recognition, not ownership.
        *pp = this;
        return S_OK;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12PipelineState)) {
        AddRef();
        *pp = static_cast<ID3D12PipelineState*>(this);
        return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dxr11RayQueryPso::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&m_refs));
}

ULONG STDMETHODCALLTYPE Dxr11RayQueryPso::Release() {
    const LONG n = InterlockedDecrement(&m_refs);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
}

// --- the rest is bookkeeping the application may call but nothing depends on -

HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::GetPrivateData(REFGUID g, UINT* s, void* d) {
    return m_so ? m_so->GetPrivateData(g, s, d) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::SetPrivateData(REFGUID g, UINT s, const void* d) {
    return m_so ? m_so->SetPrivateData(g, s, d) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::SetPrivateDataInterface(REFGUID g, const IUnknown* u) {
    return m_so ? m_so->SetPrivateDataInterface(g, u) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::SetName(LPCWSTR n) {
    return m_so ? m_so->SetName(n) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::GetDevice(REFIID riid, void** pp) {
    return m_dev ? m_dev->QueryInterface(riid, pp) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::GetCachedBlob(ID3DBlob** pp) {
    // A lowered shader has no cached blob that would mean anything to the
    // application, and handing back the real one would be a lie.
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
}
