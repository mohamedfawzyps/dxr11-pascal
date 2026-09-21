#include "rq_pipeline.h"

#include "proxy_log.h"
#include "rewriter/dxc_host.h"
#include "rewriter/ll_model.h"
#include "rewriter/rq_analyze.h"
#include "rewriter/rq_lower.h"

#include "as_tracker.h"

#include <cstring>
#include <new>
#include <string>

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
    bool hasIntersection = false;
    bool needsBoth = false;
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
    x->hasIntersection = a.query.NeedsIntersection();
    x->needsBoth = a.query.NeedsBoth();
    *out = l.text;
    return true;
}

}  // namespace

Dxr11RayQueryPso::~Dxr11RayQueryPso() {
    for (ID3D12Resource* r : m_retired) r->Release();
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

    // A hit group is one TYPE, taking an intersection shader where the other
    // takes an any-hit. A shader that commits both kinds therefore needs TWO,
    // each with its own closest-hit, because one reports committed status 1
    // and the other 2.
    D3D12_HIT_GROUP_DESC hg{};
    hg.HitGroupExport = L"HitGroup";
    hg.ClosestHitShaderImport = L"ClosestHit";
    if (x.needsBoth) {
        hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg.AnyHitShaderImport = L"AnyHit";
    } else if (x.hasIntersection) {
        hg.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        hg.IntersectionShaderImport = L"Isect";
    } else {
        hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        if (x.hasAnyHit) hg.AnyHitShaderImport = L"AnyHit";
    }

    D3D12_HIT_GROUP_DESC hgProc{};
    hgProc.HitGroupExport = L"HitGroupProc";
    hgProc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
    hgProc.IntersectionShaderImport = L"Isect";
    hgProc.ClosestHitShaderImport = L"ClosestHitProc";

    D3D12_RAYTRACING_SHADER_CONFIG sc{};
    sc.MaxPayloadSizeInBytes = kPayloadBytes;
    sc.MaxAttributeSizeInBytes = kAttrBytes;

    // Inline queries do not recurse, so one level is all a lowered shader can
    // ever need. The brief says as much.
    D3D12_RAYTRACING_PIPELINE_CONFIG pc{};
    pc.MaxTraceRecursionDepth = 1;

    D3D12_GLOBAL_ROOT_SIGNATURE grs{};
    grs.pGlobalRootSignature = desc->pRootSignature;

    // Two more hit groups that never commit, one of each type. Which one a
    // scene needs is not knowable here, so both exist and the table picks.
    // A TRIANGLES group whose any-hit always ignores sees every triangle
    // candidate and rejects it; a PROCEDURAL group whose intersection shader
    // returns reports nothing. Neither carries a closest-hit, because neither
    // can ever reach one.
    D3D12_HIT_GROUP_DESC hgNullTri{};
    hgNullTri.HitGroupExport = L"HitGroupNullTri";
    hgNullTri.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hgNullTri.AnyHitShaderImport = L"AnyHitNull";

    D3D12_HIT_GROUP_DESC hgNullProc{};
    hgNullProc.HitGroupExport = L"HitGroupNullProc";
    hgNullProc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
    hgNullProc.IntersectionShaderImport = L"IsectNull";

    D3D12_STATE_SUBOBJECT subs[8]{};
    UINT n = 0;
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg };
    if (x.needsBoth)
        subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgProc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgNullTri };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgNullProc };
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
    const void* idRay = props->GetShaderIdentifier(L"RayGen");
    const void* idMiss = props->GetShaderIdentifier(L"Miss");
    const void* idHit = props->GetShaderIdentifier(L"HitGroup");
    const void* idNullTri = props->GetShaderIdentifier(L"HitGroupNullTri");
    const void* idNullProc = props->GetShaderIdentifier(L"HitGroupNullProc");
    const void* idHitProc =
        x.needsBoth ? props->GetShaderIdentifier(L"HitGroupProc") : nullptr;
    if (!idRay || !idMiss || !idHit || !idNullTri || !idNullProc ||
        (x.needsBoth && !idHitProc)) {
        props->Release(); so->Release();
        *why = "the state object does not export RayGen, Miss, HitGroup and the "
               "two rejecting hit groups";
        return nullptr;
    }

    auto* self = new (std::nothrow) Dxr11RayQueryPso();
    if (!self) {
        props->Release(); so->Release();
        *why = "out of memory";
        return nullptr;
    }
    self->m_dev = dev;
    self->m_so = so;
    self->m_rootSig = desc->pRootSignature;
    if (self->m_rootSig) self->m_rootSig->AddRef();
    for (int i = 0; i < 3; ++i) self->m_threads[i] = static_cast<UINT>(x.threads[i]);
    // A both-kinds shader serves both; otherwise exactly one.
    self->m_servesProc = x.hasIntersection;
    self->m_servesTri = x.needsBoth || !x.hasIntersection;
    std::memcpy(self->m_idRay, idRay, kIdSize);
    std::memcpy(self->m_idMiss, idMiss, kIdSize);
    std::memcpy(self->m_idHit, idHit, kIdSize);
    std::memcpy(self->m_idNullTri, idNullTri, kIdSize);
    std::memcpy(self->m_idNullProc, idNullProc, kIdSize);
    if (idHitProc) std::memcpy(self->m_idHitProc, idHitProc, kIdSize);
    props->Release();

    // One record of the real hit group to begin with. The acceleration
    // structures have usually not been built yet at pipeline creation, so what
    // the scene needs is not knowable here; the first dispatch rebuilds the
    // table once it is.
    if (!self->BuildTable(std::vector<uint8_t>(), why)) {
        self->Release();
        return nullptr;
    }

    static LONG onceVer = 0;
    if (InterlockedCompareExchange(&onceVer, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] rewriter using %s\n", dxch::Versions());

    ProxyLog("[dxr-tier-11-proxy-log] RayQuery compute shader lowered and ready: "
             "%zu -> %zu bytes, numthreads(%u,%u,%u)%s\n",
             static_cast<size_t>(desc->CS.BytecodeLength), lib.size(),
             self->m_threads[0], self->m_threads[1], self->m_threads[2],
             x.needsBoth ? ", with a generated any-hit AND intersection shader"
                 : x.hasIntersection ? ", with a generated intersection shader"
                 : (x.hasAnyHit ? ", with a generated any-hit shader" : ""));
    return self;
}

bool Dxr11RayQueryPso::BuildTable(const std::vector<uint8_t>& kinds,
                                  std::string* why) {
    const UINT hitRecords =
        kinds.empty() ? 1u : static_cast<UINT>(kinds.size());

    // raygen, then miss, then the hit group records, each TABLE aligned to 64
    // and each RECORD to 32.
    const UINT stride = AlignUp(kIdSize, kRecAlign);
    const UINT slot = AlignUp(stride, kTableAlign);
    const UINT hitBytes = AlignUp(stride * hitRecords, kTableAlign);
    const UINT64 size = static_cast<UINT64>(slot) * 2 + hitBytes;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* sbt = nullptr;
    if (FAILED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&sbt)))) {
        if (why) *why = "cannot create the shader table buffer";
        return false;
    }

    uint8_t* p = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(sbt->Map(0, &none, reinterpret_cast<void**>(&p)))) {
        sbt->Release();
        if (why) *why = "cannot map the shader table";
        return false;
    }
    std::memset(p, 0, static_cast<size_t>(size));
    std::memcpy(p + 0 * slot, m_idRay, kIdSize);
    std::memcpy(p + 1 * slot, m_idMiss, kIdSize);

    // One record per index, of the TYPE the geometry reaching that index needs.
    // The application's contributions decide which slot a hit lands on; this
    // decides what is in the slot.
    for (UINT i = 0; i < hitRecords; ++i) {
        const uint8_t reach = i < kinds.size() ? kinds[i] : astrack::kReachNone;
        const bool tri = (reach & astrack::kReachTriangles) != 0;
        const bool proc = (reach & astrack::kReachProcedural) != 0;

        // Default to the shader's own hit group, which is right for a slot
        // reached by the kind it serves, by both when it serves only one, or
        // by nothing at all. A slot reached by BOTH when the shader serves
        // both is unservable and was refused before this call.
        const void* id = m_idHit;
        if (proc && !tri) {
            // Procedural only: the real procedural group if there is one, and
            // otherwise a procedural record that reports nothing, so the
            // geometry is traversed with a record of the right TYPE.
            id = m_servesProc ? (m_servesTri ? m_idHitProc : m_idHit)
                              : m_idNullProc;
        } else if (tri && !proc) {
            // Triangles only. m_idHit is already the triangle group whenever
            // the shader serves triangles at all.
            id = m_servesTri ? m_idHit : m_idNullTri;
        }
        std::memcpy(p + 2 * slot + i * stride, id, kIdSize);
    }
    sbt->Unmap(0, nullptr);

    if (m_sbt) m_retired.push_back(m_sbt);   // may still be in flight
    m_sbt = sbt;
    m_kinds = kinds;

    const D3D12_GPU_VIRTUAL_ADDRESS base = sbt->GetGPUVirtualAddress();
    m_desc.RayGenerationShaderRecord.StartAddress = base + 0 * slot;
    m_desc.RayGenerationShaderRecord.SizeInBytes = kIdSize;
    m_desc.MissShaderTable.StartAddress = base + 1 * slot;
    m_desc.MissShaderTable.SizeInBytes = kIdSize;
    m_desc.MissShaderTable.StrideInBytes = stride;
    m_desc.HitGroupTable.StartAddress = base + 2 * slot;
    m_desc.HitGroupTable.SizeInBytes = stride * hitRecords;
    m_desc.HitGroupTable.StrideInBytes = stride;
    return true;
}

void Dxr11RayQueryPso::DispatchAsRays(ID3D12GraphicsCommandList4* cl,
                                      UINT gx, UINT gy, UINT gz,
                                      const std::vector<uint8_t>& recordKinds) {
    if (!cl) return;

    // Rebuild when the scene's layout is not what the table was built for,
    // which is the normal case the first time: the acceleration structures did
    // not exist when the pipeline was created. A changed layout can mean more
    // records OR the same number with different types in them.
    if (!recordKinds.empty() && recordKinds != m_kinds) {
        const size_t had = m_kinds.size();
        std::string why;
        if (!BuildTable(recordKinds, &why)) {
            ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch SKIPPED: the scene "
                     "needs %u hit group records and the table could not be "
                     "rebuilt (%s).\n",
                     static_cast<unsigned>(recordKinds.size()), why.c_str());
            return;
        }
        UINT rejecting = 0;
        for (uint8_t k : recordKinds) {
            const bool tri = (k & astrack::kReachTriangles) != 0;
            const bool proc = (k & astrack::kReachProcedural) != 0;
            if (proc && !tri && !m_servesProc) ++rejecting;
            else if (tri && !proc && !m_servesTri) ++rejecting;
        }
        ProxyLog("[dxr-tier-11-proxy-log] shader table rebuilt for the scene: %u -> %u hit "
                 "group records, %u of them rejecting geometry this shader does "
                 "not serve (it commits %s).\n",
                 static_cast<unsigned>(had),
                 static_cast<unsigned>(recordKinds.size()), rejecting,
                 (m_servesTri && m_servesProc) ? "both kinds"
                     : m_servesProc ? "procedural hits" : "triangle hits");
    }

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
