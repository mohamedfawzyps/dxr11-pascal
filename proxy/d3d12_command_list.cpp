// Phase 4 (S1) - ID3D12GraphicsCommandList4 wrapper implementation.
// See d3d12_command_list.h for why the list is wrapped and the queue is hooked.
//
// Everything here is a straight forward for now. The two methods that will grow
// behaviour are ExecuteIndirect and ExecuteBundle, and ExecuteBundle already has
// to unwrap, which is a preview of the work the queue hook does.

#include "d3d12_command_list.h"
#include "geom_index_so.h"
#include "shim_scene.h"
#include "proxy_log.h"
#include "command_signature.h"
#include "rq_pipeline.h"
#include "as_tracker.h"
#include "res_tracker.h"
#include "group_count.h"
#include "dispatch_stats.h"
#include "gpu_hold.h"

#include <windows.h>
#include <cstring>
#include <string>
#include <cstdint>

#define FWD(call) m_real->call

// {A6B41C7E-2E5D-4C3B-9F80-1D4E6A0F2B91}
const GUID IID_Dxr11CommandList =
    { 0xa6b41c7e, 0x2e5d, 0x4c3b, { 0x9f, 0x80, 0x1d, 0x4e, 0x6a, 0x0f, 0x2b, 0x91 } };

void Dxr11Bindings::Retain() {
    if (rootSig) rootSig->AddRef();
    if (stateObject) stateObject->AddRef();
    for (auto* h : heaps) if (h) h->AddRef();
}
void Dxr11Bindings::ReleaseAll() {
    if (rootSig) { rootSig->Release(); rootSig = nullptr; }
    if (stateObject) { stateObject->Release(); stateObject = nullptr; }
    for (auto* h : heaps) if (h) h->Release();
    heaps.clear();
}
void Dxr11Bindings::Replay(ID3D12GraphicsCommandList4* cl) const {
    // Order matters. Descriptor heaps must be bound before any table that
    // indexes into them, and SetComputeRootSignature CLEARS the root parameters,
    // so it has to come before they are restored.
    if (!heaps.empty()) cl->SetDescriptorHeaps((UINT)heaps.size(), heaps.data());
    if (rootSig) cl->SetComputeRootSignature(rootSig);
    for (UINT i = 0; i < kMaxRootParams; ++i) {
        const Dxr11RootParam& r = roots[i];
        switch (r.kind) {
        case Dxr11RootParam::Table:     cl->SetComputeRootDescriptorTable(i, r.table); break;
        case Dxr11RootParam::CBV:       cl->SetComputeRootConstantBufferView(i, r.address); break;
        case Dxr11RootParam::SRV:       cl->SetComputeRootShaderResourceView(i, r.address); break;
        case Dxr11RootParam::UAV:       cl->SetComputeRootUnorderedAccessView(i, r.address); break;
        case Dxr11RootParam::Constants:
            if (!r.constants.empty())
                cl->SetComputeRoot32BitConstants(i, (UINT)r.constants.size(), r.constants.data(), 0);
            break;
        default: break;
        }
    }
    if (stateObject) cl->SetPipelineState1(stateObject);
}

void Dxr11GfxState::Retain() {
    if (pso) pso->AddRef();
    if (rootSig) rootSig->AddRef();
}
void Dxr11GfxState::ReleaseAll() {
    if (pso) { pso->Release(); pso = nullptr; }
    if (rootSig) { rootSig->Release(); rootSig = nullptr; }
}
void Dxr11GfxState::Replay(ID3D12GraphicsCommandList4* cl) const {
    // Same ordering rule as the compute side: the root signature clears the root
    // parameters, so it goes first. Everything is guarded by a flag recording
    // whether the application ever set it, because replaying a value it never
    // set would be an invention rather than a restoration.
    if (pso) cl->SetPipelineState(pso);
    if (rootSig) {
        cl->SetGraphicsRootSignature(rootSig);
        for (UINT i = 0; i < kMaxRootParams; ++i) {
            const Dxr11RootParam& r = roots[i];
            switch (r.kind) {
            case Dxr11RootParam::Table: cl->SetGraphicsRootDescriptorTable(i, r.table); break;
            case Dxr11RootParam::CBV:   cl->SetGraphicsRootConstantBufferView(i, r.address); break;
            case Dxr11RootParam::SRV:   cl->SetGraphicsRootShaderResourceView(i, r.address); break;
            case Dxr11RootParam::UAV:   cl->SetGraphicsRootUnorderedAccessView(i, r.address); break;
            case Dxr11RootParam::Constants:
                if (!r.constants.empty())
                    cl->SetGraphicsRoot32BitConstants(i, (UINT)r.constants.size(), r.constants.data(), 0);
                break;
            default: break;
            }
        }
    }
    if (hasTopology) cl->IASetPrimitiveTopology(topology);
    if (!viewports.empty()) cl->RSSetViewports((UINT)viewports.size(), viewports.data());
    if (!scissors.empty()) cl->RSSetScissorRects((UINT)scissors.size(), scissors.data());
    if (hasBlendFactor) cl->OMSetBlendFactor(blendFactor);
    if (hasStencilRef) cl->OMSetStencilRef(stencilRef);
    if (hasRTs) {
        cl->OMSetRenderTargets(numRTs, numRTs ? rtHandles : nullptr, rtsSingleHandle,
                               hasDSV ? &dsvHandle : nullptr);
    }
    if (hasIB) cl->IASetIndexBuffer(&ib);
    for (UINT i = 0; i < kMaxVBs; ++i)
        if (vbMask & (1ul << i)) cl->IASetVertexBuffers(i, 1, &vbs[i]);
    if (hasDepthBounds || hasViewInstanceMask) {
        ID3D12GraphicsCommandList1* cl1 = nullptr;
        if (SUCCEEDED(cl->QueryInterface(__uuidof(ID3D12GraphicsCommandList1), (void**)&cl1)) && cl1) {
            if (hasDepthBounds) cl1->OMSetDepthBounds(depthMin, depthMax);
            if (hasViewInstanceMask) cl1->SetViewInstanceMask(viewInstanceMask);
            cl1->Release();
        }
    }
}

Dxr11CommandList* Dxr11CommandList::From(ID3D12CommandList* maybe) {
    if (!maybe) return nullptr;
    Dxr11CommandList* self = nullptr;
    if (FAILED(maybe->QueryInterface(IID_Dxr11CommandList, (void**)&self)) || !self)
        return nullptr;
    self->Release();
    return self;
}

Dxr11CommandList::Dxr11CommandList(ID3D12GraphicsCommandList4* real)
    : m_real(real), m_real5(nullptr), m_real6(nullptr),
      m_real7(nullptr), m_real8(nullptr), m_real9(nullptr), m_real10(nullptr), m_refs(1) {
    if (m_real) {
        m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList5), (void**)&m_real5);
        m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList6), (void**)&m_real6);
        m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList7), (void**)&m_real7);
        m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList8), (void**)&m_real8);
        m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList9), (void**)&m_real9);
        m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList10), (void**)&m_real10);
    }
}

Dxr11CommandList::~Dxr11CommandList() {
    astrack::DropUnsubmitted(this);
    gpuhold::Detach(this);
    m_bindings.ReleaseAll();
    m_gfx.ReleaseAll();
    if (m_real10) m_real10->Release();
    if (m_real9) m_real9->Release();
    if (m_real8) m_real8->Release();
    if (m_real7) m_real7->Release();
    if (m_real6) m_real6->Release();
    if (m_real5) m_real5->Release();
    if (m_real)  m_real->Release();
}

ID3D12CommandList* Dxr11CommandList::Unwrap(ID3D12CommandList* maybeWrapped) {
    if (!maybeWrapped) return nullptr;
    Dxr11CommandList* self = nullptr;
    if (FAILED(maybeWrapped->QueryInterface(IID_Dxr11CommandList, (void**)&self)) || !self)
        return nullptr;
    ID3D12CommandList* real = self->Real();
    self->Release();
    return real;
}

HRESULT STDMETHODCALLTYPE Dxr11CommandList::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;

    // Private hand-shake so the queue hook can recognise one of ours.
    if (riid == IID_Dxr11CommandList) {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }
    if (riid == __uuidof(IUnknown)                   || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild)          || riid == __uuidof(ID3D12CommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList)  || riid == __uuidof(ID3D12GraphicsCommandList1) ||
        riid == __uuidof(ID3D12GraphicsCommandList2) || riid == __uuidof(ID3D12GraphicsCommandList3) ||
        riid == __uuidof(ID3D12GraphicsCommandList4) ||
        (riid == __uuidof(ID3D12GraphicsCommandList5) && m_real5) ||
        (riid == __uuidof(ID3D12GraphicsCommandList6) && m_real6) ||
        (riid == __uuidof(ID3D12GraphicsCommandList7) && m_real7) ||
        (riid == __uuidof(ID3D12GraphicsCommandList8) && m_real8) ||
        (riid == __uuidof(ID3D12GraphicsCommandList9) && m_real9) ||
        (riid == __uuidof(ID3D12GraphicsCommandList10) && m_real10)) {
        AddRef();
        *ppvObject = static_cast<ID3D12GraphicsCommandList10*>(this);
        return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList5) ||
        riid == __uuidof(ID3D12GraphicsCommandList6) ||
        riid == __uuidof(ID3D12GraphicsCommandList7) ||
        riid == __uuidof(ID3D12GraphicsCommandList8) ||
        riid == __uuidof(ID3D12GraphicsCommandList9) ||
        riid == __uuidof(ID3D12GraphicsCommandList10))
        return E_NOINTERFACE;

    // Anything above GraphicsCommandList6 goes out unwrapped and bypasses our
    // ExecuteIndirect. Log it: this is the same hole the device wrapper has for
    // Device8 and above, and the log is how we find out an app needs it.
    HRESULT hr = m_real->QueryInterface(riid, ppvObject);
    if (SUCCEEDED(hr))
        ProxyLog("[dxr-tier-11-proxy-log] command list QI PASSED THROUGH UNWRAPPED: %s\n",
                 ProxyIidName(riid));
    return hr;
}

ULONG STDMETHODCALLTYPE Dxr11CommandList::AddRef() { return (ULONG)InterlockedIncrement(&m_refs); }
ULONG STDMETHODCALLTYPE Dxr11CommandList::Release() {
    LONG n = InterlockedDecrement(&m_refs);
    if (n == 0) delete this;
    return (ULONG)n;
}

HRESULT STDMETHODCALLTYPE Dxr11CommandList::GetPrivateData(REFGUID guid, UINT* s, void* d) { return FWD(GetPrivateData(guid, s, d)); }
HRESULT STDMETHODCALLTYPE Dxr11CommandList::SetPrivateData(REFGUID guid, UINT s, const void* d) { return FWD(SetPrivateData(guid, s, d)); }
HRESULT STDMETHODCALLTYPE Dxr11CommandList::SetPrivateDataInterface(REFGUID guid, const IUnknown* d) { return FWD(SetPrivateDataInterface(guid, d)); }
HRESULT STDMETHODCALLTYPE Dxr11CommandList::SetName(LPCWSTR n) { return FWD(SetName(n)); }
HRESULT STDMETHODCALLTYPE Dxr11CommandList::GetDevice(REFIID riid, void** ppv) { return FWD(GetDevice(riid, ppv)); }
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE Dxr11CommandList::GetType() { return FWD(GetType()); }

HRESULT STDMETHODCALLTYPE Dxr11CommandList::Close() {
    // A dispatch queued right at the end still needs its own segment.
    FlushQueuedSplit();
    return FWD(Close());
}
// Reset starts a fresh recording, so any segments from the previous one are
// stale. It also clears all binding state, which is exactly why a split has to
// replay it.
HRESULT STDMETHODCALLTYPE Dxr11CommandList::Reset(ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
    // A copy recorded before this Reset and never submitted will never run,
    // and nothing recorded before it can run again.
    astrack::DropUnsubmitted(this);
    gpuhold::Detach(this);
    m_segments.clear();
    m_openPendings.clear();
    m_bindings.ReleaseAll();
    m_gfx.ReleaseAll();
    m_gfx = Dxr11GfxState{};
    for (UINT i = 0; i < Dxr11Bindings::kMaxRootParams; ++i)
        m_bindings.roots[i] = Dxr11RootParam{};
    m_allocator = a;
    // Reset takes an INITIAL PIPELINE STATE, and our stand-in is not a real
    // pipeline state. SetPipelineState has always trapped it; this did not,
    // and neither did ClearState or CreateCommandList, so the driver was being
    // handed an object that is not a D3D12 object at all.
    //
    // Unreal resets command lists with a PSO as a matter of course, which is
    // the ordinary way to reuse one, so this was reached on a real engine
    // immediately and never on anything in this project's own tests.
    // The carrier is a real pipeline state, so it is forwarded like any
    // other. All this does now is remember which lowered query it carries.
    m_rqPso = Dxr11RayQueryPso::From(p);
    // m_gfx was just cleared. The initial state IS the bound pipeline state,
    // and it has to be restorable after a split or a group count capture.
    if (p) { m_gfx.pso = p; p->AddRef(); }
    m_lastBoundStateObject = false;
    return FWD(Reset(a, p));
}
void STDMETHODCALLTYPE Dxr11CommandList::ClearState(ID3D12PipelineState* p) {
    WorkBarrier();
    m_rqPso = Dxr11RayQueryPso::From(p);
    if (m_gfx.pso) m_gfx.pso->Release();
    m_gfx.pso = p; if (p) p->AddRef();
    m_lastBoundStateObject = false;
    FWD(ClearState(p));
}
void STDMETHODCALLTYPE Dxr11CommandList::DrawInstanced(UINT a, UINT b, UINT c, UINT d) { WorkBarrier(); FWD(DrawInstanced(a, b, c, d)); }
void STDMETHODCALLTYPE Dxr11CommandList::DrawIndexedInstanced(UINT a, UINT b, UINT c, INT d, UINT e) { WorkBarrier(); FWD(DrawIndexedInstanced(a, b, c, d, e)); }
void STDMETHODCALLTYPE Dxr11CommandList::Dispatch(UINT x, UINT y, UINT z) {
    WorkBarrier();
    // If the bound pipeline is a lowered RayQuery shader, the application's
    // Dispatch becomes DispatchRays. Its root bindings need no translation:
    // DXR's global root signature IS the compute root signature.
    if (m_rqPso) {
        // The instance data now says whether the table the shim built can be
        // right for this scene. If it cannot, refuse rather than draw a wrong
        // image: a silently wrong result is the one failure mode this project
        // will not ship.
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> scenes;
        const bool exact = RayQueryScenes(m_rqPso, &scenes);
        const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* only = exact ? &scenes : nullptr;
        if (exact && !astrack::AnyRead(scenes)) {
            // Its instances arrive a submission late when the GPU wrote them.
            dstats::Add(dstats::kRefusedUnread);
            static LONG onceUnread = 0;
            if (InterlockedCompareExchange(&onceUnread, 1, 0) == 0)
                ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch REFUSED: the scene "
                         "it traces (0x%llX) has not been read yet. Nothing is drawn for it.\n",
                         (unsigned long long)scenes[0]);
            return;
        }
        std::string why;
        if (astrack::TableWouldBeWrong(m_rqPso->CommitsProcedural(), &why, only)) {
            static LONG once = 0;
            if (InterlockedCompareExchange(&once, 1, 0) == 0)
                ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch REFUSED: %s. "
                         "Nothing is drawn for it.\n", why.c_str());
            return;
        }
        // What geometry reaches each hit group record index, read from the
        // instance descriptions. Empty until a top-level structure has been
        // seen, which is the right answer for a scene that has none.
        m_rqPso->DispatchAsRays(m_real, x, y, z, astrack::RecordKinds(only), this, only);
        return;
    }
    FWD(Dispatch(x, y, z));
}
void STDMETHODCALLTYPE Dxr11CommandList::CopyBufferRegion(ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT64 n) { WorkBarrier(); FWD(CopyBufferRegion(d, dof, s, sof, n)); }
void STDMETHODCALLTYPE Dxr11CommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* b) { WorkBarrier(); FWD(CopyTextureRegion(d, x, y, z, s, b)); }
void STDMETHODCALLTYPE Dxr11CommandList::CopyResource(ID3D12Resource* d, ID3D12Resource* s) { WorkBarrier(); FWD(CopyResource(d, s)); }
void STDMETHODCALLTYPE Dxr11CommandList::CopyTiles(ID3D12Resource* r, const D3D12_TILED_RESOURCE_COORDINATE* c, const D3D12_TILE_REGION_SIZE* sz, ID3D12Resource* b, UINT64 off, D3D12_TILE_COPY_FLAGS f) { WorkBarrier(); FWD(CopyTiles(r, c, sz, b, off, f)); }
void STDMETHODCALLTYPE Dxr11CommandList::ResolveSubresource(ID3D12Resource* d, UINT ds, ID3D12Resource* s, UINT ss, DXGI_FORMAT f) { WorkBarrier(); FWD(ResolveSubresource(d, ds, s, ss, f)); }
void STDMETHODCALLTYPE Dxr11CommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t) {
    m_gfx.hasTopology = true; m_gfx.topology = t;
    FWD(IASetPrimitiveTopology(t));
}
void STDMETHODCALLTYPE Dxr11CommandList::RSSetViewports(UINT n, const D3D12_VIEWPORT* v) {
    if (v) m_gfx.viewports.assign(v, v + n); else m_gfx.viewports.clear();
    FWD(RSSetViewports(n, v));
}
void STDMETHODCALLTYPE Dxr11CommandList::RSSetScissorRects(UINT n, const D3D12_RECT* r) {
    if (r) m_gfx.scissors.assign(r, r + n); else m_gfx.scissors.clear();
    FWD(RSSetScissorRects(n, r));
}
void STDMETHODCALLTYPE Dxr11CommandList::OMSetBlendFactor(const FLOAT f[4]) {
    if (f) { m_gfx.hasBlendFactor = true; std::memcpy(m_gfx.blendFactor, f, sizeof(m_gfx.blendFactor)); }
    FWD(OMSetBlendFactor(f));
}
void STDMETHODCALLTYPE Dxr11CommandList::OMSetStencilRef(UINT s) {
    m_gfx.hasStencilRef = true; m_gfx.stencilRef = s;
    FWD(OMSetStencilRef(s));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetPipelineState(ID3D12PipelineState* p) {
    // Our stand-in is not a real pipeline state and must never reach the
    // driver. Remember it; Dispatch is where it does its work.
    // Remember the lowered query, then bind the carrier as usual. It is a
    // real pipeline state and binding it is harmless: Dispatch replaces the
    // work with SetPipelineState1 and DispatchRays anyway.
    m_rqPso = Dxr11RayQueryPso::From(p);
    if (m_gfx.pso) m_gfx.pso->Release();
    m_gfx.pso = p; if (p) p->AddRef();
    m_lastBoundStateObject = false;
    FWD(SetPipelineState(p));
}
void STDMETHODCALLTYPE Dxr11CommandList::ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b) { WorkBarrier(); FWD(ResourceBarrier(n, b)); }

// A bundle is created through CreateCommandList too, so it arrives wrapped.
// Unwrap before the runtime sees it. Same problem the queue hook solves, in
// miniature.
void STDMETHODCALLTYPE Dxr11CommandList::ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) {
    WorkBarrier();
    ID3D12CommandList* real = Dxr11CommandList::Unwrap(pCommandList);
    FWD(ExecuteBundle(real ? static_cast<ID3D12GraphicsCommandList*>(real) : pCommandList));
}

// --- binding tracking -------------------------------------------------------
// Recorded so it can be replayed after a split. Every one of these is still a
// faithful forward; the bookkeeping is pure addition.
void STDMETHODCALLTYPE Dxr11CommandList::SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* h) {
    for (auto* old : m_bindings.heaps) if (old) old->Release();
    m_bindings.heaps.assign(h, h + n);
    for (auto* nh : m_bindings.heaps) if (nh) nh->AddRef();
    FWD(SetDescriptorHeaps(n, h));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRootSignature(ID3D12RootSignature* r) {
    if (m_bindings.rootSig) m_bindings.rootSig->Release();
    m_bindings.rootSig = r;
    if (r) r->AddRef();
    // Setting a root signature clears the parameters, so forget them too.
    for (UINT i = 0; i < Dxr11Bindings::kMaxRootParams; ++i)
        m_bindings.roots[i] = Dxr11RootParam{};
    FWD(SetComputeRootSignature(r));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRootSignature(ID3D12RootSignature* r) {
    if (m_gfx.rootSig) m_gfx.rootSig->Release();
    m_gfx.rootSig = r; if (r) r->AddRef();
    for (UINT i = 0; i < Dxr11GfxState::kMaxRootParams; ++i) m_gfx.roots[i] = Dxr11RootParam{};
    FWD(SetGraphicsRootSignature(r));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (i < Dxr11Bindings::kMaxRootParams) { m_bindings.roots[i].kind = Dxr11RootParam::Table; m_bindings.roots[i].table = h; }
    FWD(SetComputeRootDescriptorTable(i, h));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (i < Dxr11GfxState::kMaxRootParams) { m_gfx.roots[i].kind = Dxr11RootParam::Table; m_gfx.roots[i].table = h; }
    FWD(SetGraphicsRootDescriptorTable(i, h));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRoot32BitConstant(UINT i, UINT v, UINT o) {
    if (i < Dxr11Bindings::kMaxRootParams) {
        Dxr11RootParam& r = m_bindings.roots[i];
        r.kind = Dxr11RootParam::Constants;
        if (r.constants.size() < (size_t)o + 1) r.constants.resize((size_t)o + 1, 0);
        r.constants[o] = v;
    }
    FWD(SetComputeRoot32BitConstant(i, v, o));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRoot32BitConstant(UINT i, UINT v, UINT o) {
    if (i < Dxr11GfxState::kMaxRootParams) {
        Dxr11RootParam& r = m_gfx.roots[i];
        r.kind = Dxr11RootParam::Constants;
        if (r.constants.size() < (size_t)o + 1) r.constants.resize((size_t)o + 1, 0);
        r.constants[o] = v;
    }
    FWD(SetGraphicsRoot32BitConstant(i, v, o));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRoot32BitConstants(UINT i, UINT n, const void* d, UINT o) {
    if (i < Dxr11Bindings::kMaxRootParams && d) {
        Dxr11RootParam& r = m_bindings.roots[i];
        r.kind = Dxr11RootParam::Constants;
        if (r.constants.size() < (size_t)o + n) r.constants.resize((size_t)o + n, 0);
        std::memcpy(r.constants.data() + o, d, (size_t)n * sizeof(UINT));
    }
    FWD(SetComputeRoot32BitConstants(i, n, d, o));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRoot32BitConstants(UINT i, UINT n, const void* d, UINT o) {
    if (i < Dxr11GfxState::kMaxRootParams && d) {
        Dxr11RootParam& r = m_gfx.roots[i];
        r.kind = Dxr11RootParam::Constants;
        if (r.constants.size() < (size_t)o + n) r.constants.resize((size_t)o + n, 0);
        std::memcpy(r.constants.data() + o, d, (size_t)n * sizeof(UINT));
    }
    FWD(SetGraphicsRoot32BitConstants(i, n, d, o));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    if (i < Dxr11Bindings::kMaxRootParams) { m_bindings.roots[i].kind = Dxr11RootParam::CBV; m_bindings.roots[i].address = a; }
    FWD(SetComputeRootConstantBufferView(i, a));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    if (i < Dxr11GfxState::kMaxRootParams) { m_gfx.roots[i].kind = Dxr11RootParam::CBV; m_gfx.roots[i].address = a; }
    FWD(SetGraphicsRootConstantBufferView(i, a));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    if (i < Dxr11Bindings::kMaxRootParams) { m_bindings.roots[i].kind = Dxr11RootParam::SRV; m_bindings.roots[i].address = a; }
    FWD(SetComputeRootShaderResourceView(i, a));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    if (i < Dxr11GfxState::kMaxRootParams) { m_gfx.roots[i].kind = Dxr11RootParam::SRV; m_gfx.roots[i].address = a; }
    FWD(SetGraphicsRootShaderResourceView(i, a));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    if (i < Dxr11Bindings::kMaxRootParams) { m_bindings.roots[i].kind = Dxr11RootParam::UAV; m_bindings.roots[i].address = a; }
    FWD(SetComputeRootUnorderedAccessView(i, a));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    if (i < Dxr11GfxState::kMaxRootParams) { m_gfx.roots[i].kind = Dxr11RootParam::UAV; m_gfx.roots[i].address = a; }
    FWD(SetGraphicsRootUnorderedAccessView(i, a));
}
void STDMETHODCALLTYPE Dxr11CommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) {
    if (v) { m_gfx.hasIB = true; m_gfx.ib = *v; } else { m_gfx.hasIB = false; }
    FWD(IASetIndexBuffer(v));
}
void STDMETHODCALLTYPE Dxr11CommandList::IASetVertexBuffers(UINT s, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    for (UINT i = 0; i < n; ++i) {
        const UINT slot = s + i;
        if (slot >= Dxr11GfxState::kMaxVBs) break;
        if (v) { m_gfx.vbs[slot] = v[i]; m_gfx.vbMask |= (1ul << slot); }
        else   { m_gfx.vbMask &= ~(1ul << slot); }
    }
    FWD(IASetVertexBuffers(s, n, v));
}
void STDMETHODCALLTYPE Dxr11CommandList::SOSetTargets(UINT s, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* v) { FWD(SOSetTargets(s, n, v)); }
void STDMETHODCALLTYPE Dxr11CommandList::OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rt, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* ds) {
    m_gfx.hasRTs = true;
    m_gfx.rtsSingleHandle = single;
    m_gfx.numRTs = n;
    // With a single handle the array holds ONE entry describing a contiguous
    // range, so copying n of them would read past the end.
    const UINT copy = single ? (n ? 1u : 0u) : n;
    for (UINT i = 0; i < copy && i < Dxr11GfxState::kMaxRTs; ++i)
        if (rt) m_gfx.rtHandles[i] = rt[i];
    m_gfx.hasDSV = (ds != nullptr);
    if (ds) m_gfx.dsvHandle = *ds;
    FWD(OMSetRenderTargets(n, rt, single, ds));
}
void STDMETHODCALLTYPE Dxr11CommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE v, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s, UINT n, const D3D12_RECT* r) { WorkBarrier(); FWD(ClearDepthStencilView(v, f, d, s, n, r)); }
void STDMETHODCALLTYPE Dxr11CommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE v, const FLOAT c[4], UINT n, const D3D12_RECT* r) { WorkBarrier(); FWD(ClearRenderTargetView(v, c, n, r)); }
void STDMETHODCALLTYPE Dxr11CommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res, const UINT v[4], UINT n, const D3D12_RECT* r) { WorkBarrier(); FWD(ClearUnorderedAccessViewUint(g, c, res, v, n, r)); }
void STDMETHODCALLTYPE Dxr11CommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res, const FLOAT v[4], UINT n, const D3D12_RECT* r) { WorkBarrier(); FWD(ClearUnorderedAccessViewFloat(g, c, res, v, n, r)); }
void STDMETHODCALLTYPE Dxr11CommandList::DiscardResource(ID3D12Resource* r, const D3D12_DISCARD_REGION* d) { WorkBarrier(); FWD(DiscardResource(r, d)); }
void STDMETHODCALLTYPE Dxr11CommandList::BeginQuery(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) { WorkBarrier(); FWD(BeginQuery(h, t, i)); }
void STDMETHODCALLTYPE Dxr11CommandList::EndQuery(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) { WorkBarrier(); FWD(EndQuery(h, t, i)); }
void STDMETHODCALLTYPE Dxr11CommandList::ResolveQueryData(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT s, UINT n, ID3D12Resource* d, UINT64 o) { WorkBarrier(); FWD(ResolveQueryData(h, t, s, n, d, o)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetPredication(ID3D12Resource* b, UINT64 o, D3D12_PREDICATION_OP op) { WorkBarrier(); FWD(SetPredication(b, o, op)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetMarker(UINT m, const void* d, UINT s) { FWD(SetMarker(m, d, s)); }
void STDMETHODCALLTYPE Dxr11CommandList::BeginEvent(UINT m, const void* d, UINT s) { FWD(BeginEvent(m, d, s)); }
void STDMETHODCALLTYPE Dxr11CommandList::EndEvent() { FWD(EndEvent()); }

// The seat this whole wrapper exists to create.
//
// A signature the runtime built is forwarded untouched. One of our DISPATCH_RAYS
// stand-ins has to be serviced here, because Tier 1.0 has no indirect ray
// dispatch at all.
//
// PARTIAL. Only the case where the argument buffer is CPU-visible and already
// final at record time is implemented. That is enough to prove the whole chain,
// signature through to a real dispatch, and it is NOT the case Unreal uses:
// Unreal builds its argument buffer on the GPU immediately before the dispatch,
// so nothing in it is valid yet at this point. That needs the command list
// split, which is the next piece of work. Until then the GPU-written case
// refuses loudly rather than dispatching something wrong.
void STDMETHODCALLTYPE Dxr11CommandList::ExecuteIndirect(ID3D12CommandSignature* sig, UINT maxCount, ID3D12Resource* args, UINT64 argOffset, ID3D12Resource* countBuf, UINT64 countOffset) {
    Dxr11CommandSignature* ours = Dxr11CommandSignature::From(sig);
    if (!ours && m_rqPso) {
        // A compute DispatchIndirect with a lowered RayQuery pipeline bound.
        // Forwarding it would run the carrier, which does nothing, silently.
        // That is how Unreal dispatches most of its Lumen and MegaLights
        // inline passes. See proxy/group_count.h.
        UINT stride = 0;
        const groupcount::Shape shape = groupcount::SignatureShape(sig, &stride);
        const char* refuse = nullptr;
        if (shape != groupcount::Shape::kDispatch)
            refuse = shape == groupcount::Shape::kUnsupported
                ? "its signature has other arguments or a root signature besides the DISPATCH"
                : "its signature is not a DISPATCH signature this shim saw created";
        else if (countBuf) refuse = "a count buffer is not supported";
        else if (!args) refuse = "the argument buffer is null";
        if (refuse) {
            static LONG once = 0;
            if (InterlockedCompareExchange(&once, 1, 0) == 0)
                ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect on a lowered RayQuery pipeline "
                         "NOT EMULATED: %s. Nothing is drawn for it.\n", refuse);
            return;
        }
        if (!stride) stride = (UINT)sizeof(D3D12_DISPATCH_ARGUMENTS);

        D3D12_HEAP_PROPERTIES heap{};
        D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
        const bool cpuVisible =
            SUCCEEDED(args->GetHeapProperties(&heap, &heapFlags)) &&
            (heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_READBACK);
        for (UINT i = 0; i < maxCount; ++i) {
            const UINT64 offset = argOffset + (UINT64)i * stride;
            if (cpuVisible) {
                // The arguments exist already: read them and take the direct
                // path, which is exactly what a Dispatch would do.
                D3D12_DISPATCH_ARGUMENTS da{};
                uint8_t* p = nullptr;
                D3D12_RANGE readRange{ (SIZE_T)offset, (SIZE_T)(offset + sizeof(da)) };
                if (FAILED(args->Map(0, &readRange, (void**)&p)) || !p) {
                    ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect(DISPATCH) on a lowered "
                             "RayQuery pipeline: could not map the argument buffer, "
                             "dispatch SKIPPED\n");
                    return;
                }
                std::memcpy(&da, p + offset, sizeof(da));
                D3D12_RANGE noWrite{ 0, 0 };
                args->Unmap(0, &noWrite);
                static LONG onceCpu = 0;
                if (InterlockedCompareExchange(&onceCpu, 1, 0) == 0)
                    ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect(DISPATCH) on a lowered "
                             "RayQuery pipeline -> DispatchRays, %ux%ux%u groups read from a "
                             "CPU-visible argument buffer at record time\n",
                             da.ThreadGroupCountX, da.ThreadGroupCountY, da.ThreadGroupCountZ);
                if (da.ThreadGroupCountX && da.ThreadGroupCountY && da.ThreadGroupCountZ)
                    Dispatch(da.ThreadGroupCountX, da.ThreadGroupCountY, da.ThreadGroupCountZ);
                continue;
            }
            if (!QueueIndirectCompute(sig, args, offset)) {
                ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect(DISPATCH) on a lowered RayQuery "
                         "pipeline: group count capture failed, dispatch SKIPPED\n");
                return;
            }
        }
        return;
    }
    if (!ours) {
        WorkBarrier();
        FWD(ExecuteIndirect(sig, maxCount, args, argOffset, countBuf, countOffset));
        return;
    }

    if (countBuf) {
        ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect(DISPATCH_RAYS): a count buffer is not "
                 "supported yet, dispatch SKIPPED\n");
        return;
    }
    if (!args) {
        ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect(DISPATCH_RAYS): null argument buffer\n");
        return;
    }

    // Can the CPU see the arguments at all? A DEFAULT heap means they are
    // produced on the GPU and do not exist yet, which is Unreal's case and needs
    // the command list split.
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
    const bool cpuVisible =
        SUCCEEDED(args->GetHeapProperties(&heap, &heapFlags)) &&
        (heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_READBACK);

    if (!cpuVisible) {
        if (maxCount != 1) {
            ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect(DISPATCH_RAYS): MaxCommandCount=%u with a "
                     "GPU-written argument buffer is not supported, dispatch SKIPPED\n", maxCount);
            return;
        }
        if (!QueueSplit(args, argOffset)) {
            ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect(DISPATCH_RAYS): split failed, "
                     "dispatch SKIPPED\n");
        }
        return;
    }

    const UINT stride = ours->ByteStride() ? ours->ByteStride()
                                           : (UINT)sizeof(D3D12_DISPATCH_RAYS_DESC);
    for (UINT i = 0; i < maxCount; ++i) {
        D3D12_DISPATCH_RAYS_DESC desc{};
        uint8_t* p = nullptr;
        const UINT64 offset = argOffset + (UINT64)i * stride;
        D3D12_RANGE readRange{ (SIZE_T)offset, (SIZE_T)(offset + sizeof(desc)) };
        if (FAILED(args->Map(0, &readRange, (void**)&p)) || !p) {
            ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect(DISPATCH_RAYS): could not map the "
                     "argument buffer, dispatch SKIPPED\n");
            return;
        }
        std::memcpy(&desc, p + offset, sizeof(desc));
        D3D12_RANGE noWrite{ 0, 0 };
        args->Unmap(0, &noWrite);

        static LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) == 0)
            ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect(DISPATCH_RAYS) -> direct DispatchRays "
                     "%ux%ux%u, read from a CPU-visible argument buffer at record time\n",
                     desc.Width, desc.Height, desc.Depth);

        if (desc.Width && desc.Height && desc.Depth) FWD(DispatchRays(&desc));
    }
}

void STDMETHODCALLTYPE Dxr11CommandList::AtomicCopyBufferUINT(ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT n, ID3D12Resource* const* dep, const D3D12_SUBRESOURCE_RANGE_UINT64* ranges) { WorkBarrier(); FWD(AtomicCopyBufferUINT(d, dof, s, sof, n, dep, ranges)); }
void STDMETHODCALLTYPE Dxr11CommandList::AtomicCopyBufferUINT64(ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT n, ID3D12Resource* const* dep, const D3D12_SUBRESOURCE_RANGE_UINT64* ranges) { WorkBarrier(); FWD(AtomicCopyBufferUINT64(d, dof, s, sof, n, dep, ranges)); }
void STDMETHODCALLTYPE Dxr11CommandList::OMSetDepthBounds(FLOAT a, FLOAT b) {
    m_gfx.hasDepthBounds = true; m_gfx.depthMin = a; m_gfx.depthMax = b;
    FWD(OMSetDepthBounds(a, b));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetSamplePositions(UINT a, UINT b, D3D12_SAMPLE_POSITION* p) { FWD(SetSamplePositions(a, b, p)); }
void STDMETHODCALLTYPE Dxr11CommandList::ResolveSubresourceRegion(ID3D12Resource* d, UINT ds, UINT x, UINT y, ID3D12Resource* s, UINT ss, D3D12_RECT* r, DXGI_FORMAT f, D3D12_RESOLVE_MODE m) { WorkBarrier(); FWD(ResolveSubresourceRegion(d, ds, x, y, s, ss, r, f, m)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetViewInstanceMask(UINT m) {
    m_gfx.hasViewInstanceMask = true; m_gfx.viewInstanceMask = m;
    FWD(SetViewInstanceMask(m));
}
void STDMETHODCALLTYPE Dxr11CommandList::WriteBufferImmediate(UINT c, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* p, const D3D12_WRITEBUFFERIMMEDIATE_MODE* m) { WorkBarrier(); FWD(WriteBufferImmediate(c, p, m)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetProtectedResourceSession(ID3D12ProtectedResourceSession* s) { FWD(SetProtectedResourceSession(s)); }
void STDMETHODCALLTYPE Dxr11CommandList::BeginRenderPass(UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS f) { WorkBarrier(); FWD(BeginRenderPass(n, rt, ds, f)); }
void STDMETHODCALLTYPE Dxr11CommandList::EndRenderPass() { WorkBarrier(); FWD(EndRenderPass()); }
void STDMETHODCALLTYPE Dxr11CommandList::InitializeMetaCommand(ID3D12MetaCommand* m, const void* d, SIZE_T s) { WorkBarrier(); FWD(InitializeMetaCommand(m, d, s)); }
void STDMETHODCALLTYPE Dxr11CommandList::ExecuteMetaCommand(ID3D12MetaCommand* m, const void* d, SIZE_T s) { WorkBarrier(); FWD(ExecuteMetaCommand(m, d, s)); }
void STDMETHODCALLTYPE Dxr11CommandList::BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT n, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* p) {
    WorkBarrier();
    // Bottom-level geometry descriptions arrive as CPU memory, so the geometry
    // type of every structure can be learned here for nothing. The shader
    // table needs it: a record has to match the geometry that resolves to it.
    astrack::NoteBuild(d);
    if (d && d->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
        CaptureInstances(d);
    // Every bottom-level geometry gets NO_DUPLICATE_ANYHIT_INVOCATION, the
    // same as in the prebuild query; see astrack::NoDuplicateAnyHit.
    if (d) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in2;
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoms;
        const auto* in = astrack::NoDuplicateAnyHit(&d->Inputs, &in2, &geoms);
        if (in != &d->Inputs) {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d2 = *d;
            d2.Inputs = *in;
            FWD(BuildRaytracingAccelerationStructure(&d2, n, p));
            return;
        }
    }
    FWD(BuildRaytracingAccelerationStructure(d, n, p));
    // The shim's copy of the scene, when a variant pipeline needs one: built
    // here, from the instance buffer the application just built from.
    if (d && d->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL &&
        shimscene::Active()) {
        std::string why;
        ID3D12Device5* dev = RealDevice();
        if (!dev || !shimscene::Record(m_real, dev, *d, this, &why)) {
            static LONG k = 0;
            if (InterlockedIncrement(&k) <= 8)
                ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): scene copy not built: %s\n",
                         why.empty() ? "no device" : why.c_str());
        }
        RestoreComputeAfterCapture();
    }
}
void STDMETHODCALLTYPE Dxr11CommandList::EmitRaytracingAccelerationStructurePostbuildInfo(const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d, UINT n, const D3D12_GPU_VIRTUAL_ADDRESS* a) { WorkBarrier(); FWD(EmitRaytracingAccelerationStructurePostbuildInfo(d, n, a)); }
void STDMETHODCALLTYPE Dxr11CommandList::CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS d, D3D12_GPU_VIRTUAL_ADDRESS s, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE m) { WorkBarrier(); FWD(CopyRaytracingAccelerationStructure(d, s, m)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetPipelineState1(ID3D12StateObject* s) {
    if (m_bindings.stateObject) m_bindings.stateObject->Release();
    m_bindings.stateObject = s;
    if (s) s->AddRef();
    m_lastBoundStateObject = true;
    FWD(SetPipelineState1(s));
}
bool Dxr11CommandList::ResolveBoundScenes(const gidx::Scenes& sc,
                                          std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* out,
                                          std::string* why) {
    std::vector<gidx::BoundRoot> roots(Dxr11Bindings::kMaxRootParams);
    for (UINT i = 0; i < Dxr11Bindings::kMaxRootParams; ++i) {
        const auto& r = m_bindings.roots[i];
        if (r.kind == Dxr11RootParam::SRV) roots[i].srv = r.address;
        if (r.kind == Dxr11RootParam::CBV) roots[i].cbv = r.address;
        if (r.kind == Dxr11RootParam::Table) roots[i].table = r.table.ptr;
        if (r.kind == Dxr11RootParam::Constants) {
            roots[i].constants = r.constants.data();
            roots[i].numConstants = (UINT)r.constants.size();
        }
    }
    return gidx::ResolveScenes(sc, m_bindings.rootSig, roots, m_bindings.heaps.data(),
                               (UINT)m_bindings.heaps.size(), out, why);
}

// The scene a lowered RayQuery dispatch traces. Resolved, its table is built
// from that scene alone; otherwise from every live one, as before 0.49.0,
// which can refuse a dispatch two live scenes disagree about but never draws
// one wrong.
bool Dxr11CommandList::RayQueryScenes(Dxr11RayQueryPso* rq,
                                      std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* out) {
    std::string why;
    if (ResolveBoundScenes(rq->Scenes(), out, &why)) {
        dstats::Add(dstats::kSceneResolved);
        return true;
    }
    out->clear();
    dstats::Add(dstats::kSceneUnresolved);
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 8)
        ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch: the scene it traces is not "
                 "resolved (%s); judged over every live scene\n", why.c_str());
    return false;
}

// A pipeline whose hit shaders read GeometryIndex() dispatches against the
// shim's copy of the hit group table, which carries each record's geometry
// index; see proxy/geom_index_so.h. A dispatch that copy cannot serve is NOT
// recorded, and says so: drawing it against the application's table would
// run hit shaders with no geometry index at all.
void STDMETHODCALLTYPE Dxr11CommandList::DispatchRays(const D3D12_DISPATCH_RAYS_DESC* d) {
    WorkBarrier();
    std::shared_ptr<gidx::Info> gi = d ? gidx::Get(m_bindings.stateObject) : nullptr;
    if (!gi) { FWD(DispatchRays(d)); return; }
    ID3D12Device5* dev = RealDevice();
    D3D12_DISPATCH_RAYS_DESC mine = *d;
    std::string why;
    // The scene the dispatch traces: resolved through the bound root
    // signature when every TraceRay's scene can be, root SRV, descriptor
    // table or heap index. Otherwise every live scene, which can refuse a
    // layout that was fine but never draws wrong. (Until 0.48.0 a bound root
    // SRV that was a known scene counted as THE scene; wrong when the shader
    // traces another one, a heap-indexed one say, beside it.)
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> srvs;
    std::string swhy;
    const bool exact = ResolveBoundScenes(gi->scenes, &srvs, &swhy);
    if (!exact) {
        srvs.clear();
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 4)
            ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): the scene this dispatch traces is not "
                     "resolved (%s); judged over every live scene\n", swhy.c_str());
    }
    bool shared = false;
    if (!dev || !gidx::RecordTable(m_real, dev, *gi, d->HitGroupTable, &mine.HitGroupTable,
                                   srvs, exact, this, &shared, &why)) {
        // A record several geometries reach: the shim's own layout, through
        // the variant pipeline and the shim's copy of the scene.
        std::string vwhy;
        if (dev && shared && gidx::EnsureVariant(dev, *gi, m_bindings.stateObject, &vwhy) &&
            gidx::RecordVariant(m_real, dev, *gi, *d, &mine, srvs, exact, this, &vwhy)) {
            RestoreComputeAfterCapture();
            m_real->SetPipelineState1(gi->variant.Get());
            m_real->DispatchRays(&mine);
            m_real->SetPipelineState1(m_bindings.stateObject);
            static LONG firstVariant = 0;
            if (InterlockedCompareExchange(&firstVariant, 1, 0) == 0)
                ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): first dispatch in the shim's own "
                         "layout (%s), %llu hit group records\n", why.c_str(),
                         (unsigned long long)(mine.HitGroupTable.SizeInBytes /
                                              mine.HitGroupTable.StrideInBytes));
            return;
        }
        if (shared) why += "; the shim's own layout could not serve it either: " + vwhy;
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 16)
            ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex() dispatch NOT DRAWN: %s\n",
                     why.empty() ? "no device" : why.c_str());
        RestoreComputeAfterCapture();
        return;
    }
    RestoreComputeAfterCapture();
    static LONG first = 0;
    if (InterlockedCompareExchange(&first, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): first dispatch against the shim's "
                 "table, %llu records at stride %llu (the application's: stride %llu)\n",
                 (unsigned long long)(mine.HitGroupTable.SizeInBytes /
                                      mine.HitGroupTable.StrideInBytes),
                 (unsigned long long)mine.HitGroupTable.StrideInBytes,
                 (unsigned long long)d->HitGroupTable.StrideInBytes);
    m_real->DispatchRays(&mine);
}

// --- ID3D12GraphicsCommandList5 / 6 -----------------------------------------
// Only reachable when QueryInterface handed the interface out, which it does
// only when the corresponding pointer exists. The null checks are belt and
// braces.

void STDMETHODCALLTYPE Dxr11CommandList::RSSetShadingRate(D3D12_SHADING_RATE r, const D3D12_SHADING_RATE_COMBINER* c) {
    if (m_real5) m_real5->RSSetShadingRate(r, c);
}
void STDMETHODCALLTYPE Dxr11CommandList::RSSetShadingRateImage(ID3D12Resource* img) {
    if (m_real5) m_real5->RSSetShadingRateImage(img);
}
void STDMETHODCALLTYPE Dxr11CommandList::DispatchMesh(UINT x, UINT y, UINT z) { WorkBarrier();
    if (m_real6) m_real6->DispatchMesh(x, y, z);
}

// --- splitting ---------------------------------------------------------------

ID3D12Device5* Dxr11CommandList::RealDevice() {
    if (!m_device) {
        // Ask the REAL list, so this never re-enters the device wrapper.
        m_real->GetDevice(__uuidof(ID3D12Device5), (void**)m_device.GetAddressOf());
    }
    return m_device.Get();
}

void Dxr11CommandList::CaptureInstances(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc) {
    const auto& in = desc->Inputs;
    // Whatever the previous build of this structure was, it is not this one.
    astrack::DropSnapshot(desc->DestAccelerationStructureData);
    if (!in.NumDescs || !in.InstanceDescs) return;

    if (in.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY) {
        static LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) == 0)
            ProxyLog("[dxr-tier-11-proxy-log] top-level AS: ARRAY_OF_POINTERS instance "
                     "descriptions are not read (each pointer is itself a GPU "
                     "address, needing a second dependent copy).\n");
        return;
    }

    // Upload-heap descriptions are read in place, at record time, for free. The
    // heap type was recorded when the resource was created, so deciding this
    // calls into nothing that may since have been freed.
    const restrack::Found src = restrack::Find(in.InstanceDescs);
    const bool cheap = src.resource &&
        (src.heap == D3D12_HEAP_TYPE_UPLOAD || src.heap == D3D12_HEAP_TYPE_READBACK);

    // Read again when the structure may have changed. See WantInstances for
    // what once-per-address did to a level load.
    if (!astrack::WantInstances(desc->DestAccelerationStructureData, in.NumDescs, cheap))
        return;

    if (cheap) {
        const UINT64 bytes =
            static_cast<UINT64>(in.NumDescs) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        uint8_t* p = nullptr;
        D3D12_RANGE readRange{ static_cast<SIZE_T>(src.offset),
                               static_cast<SIZE_T>(src.offset + bytes) };
        if (SUCCEEDED(src.resource->Map(0, &readRange, (void**)&p)) && p) {
            astrack::NoteInstances(
                desc->DestAccelerationStructureData,
                reinterpret_cast<const D3D12_RAYTRACING_INSTANCE_DESC*>(p + src.offset),
                in.NumDescs);
            D3D12_RANGE noWrite{ 0, 0 };
            src.resource->Unmap(0, &noWrite);
        }
        return;
    }

    // Produced on the GPU, so they do not exist yet. Copy them out by ADDRESS,
    // through a root SRV, into buffers the shim owns, and let the queue hook
    // read them once the submission has run. No resource is looked up and
    // none is transitioned: 0.39.1 recorded a barrier and a copy on whatever
    // the tracker held at that address, which during a level load can be a
    // resource the application already freed, and the list then failed Close
    // with E_INVALIDARG. See proxy/instance_copy.hlsl.
    ID3D12Device5* dev = RealDevice();
    if (!dev) return;
    groupcount::Capture cap;
    std::string why;
    const UINT dwords = in.NumDescs *
        static_cast<UINT>(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) / sizeof(UINT));
    if (!groupcount::RecordRawCopy(m_real, dev, in.InstanceDescs, dwords, &cap, &why)) {
        static LONG once = 0;
        if (InterlockedCompareExchange(&once, 1, 0) == 0)
            ProxyLog("[dxr-tier-11-proxy-log] top-level AS: instance copy not recorded: %s\n",
                     why.c_str());
        return;
    }
    RestoreComputeAfterCapture();
    astrack::NotePendingInstances(desc->DestAccelerationStructureData,
                                  cap.readback.Get(), in.NumDescs, this, cap.counts.Get());
}

void Dxr11CommandList::RestoreComputeAfterCapture() {
    // A capture replaced the compute root signature, which clears every root
    // parameter, and the pipeline. Give the application both back, the
    // pipeline it bound LAST last, since a state object and a pipeline state
    // replace each other.
    m_bindings.Replay(m_real);
    if (m_lastBoundStateObject) {
        if (m_bindings.stateObject) m_real->SetPipelineState1(m_bindings.stateObject);
    } else if (m_gfx.pso) {
        m_real->SetPipelineState(m_gfx.pso);
    }
}

bool Dxr11CommandList::QueueSplit(ID3D12Resource* args, UINT64 argOffset) {
    ID3D12Device5* dev = RealDevice();
    if (!dev || !m_allocator) {
        ProxyLog("[dxr-tier-11-proxy-log] split: no device or allocator (was Reset called?)\n");
        return false;
    }

    // Somewhere to put the arguments once the GPU has produced them.
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = sizeof(D3D12_DISPATCH_RAYS_DESC);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
        ProxyLog("[dxr-tier-11-proxy-log] split: could not create the readback buffer\n");
        return false;
    }

    // The barriers are NOT optional. D3D12 requires the argument buffer to be in
    // INDIRECT_ARGUMENT state at an ExecuteIndirect, and whatever wrote it needs
    // an execution dependency before this read. Without them the copy returns
    // stale contents, which showed up as dimensions of 0x0x0.
    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = args;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_real->ResourceBarrier(1, &toCopy);

    m_real->CopyBufferRegion(readback.Get(), 0, args, argOffset, sizeof(D3D12_DISPATCH_RAYS_DESC));

    D3D12_RESOURCE_BARRIER back = toCopy;
    back.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    back.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    m_real->ResourceBarrier(1, &back);

    // Queue it, but do NOT close the segment yet. If the next thing the
    // application records is another indirect ray dispatch, both land in this
    // same segment and share a single sync.
    Dxr11PendingDispatch pend;
    pend.readback = readback;
    pend.bindings = m_bindings;
    pend.bindings.Retain();
    m_openPendings.push_back(pend);
    return true;
}

bool Dxr11CommandList::QueueIndirectCompute(ID3D12CommandSignature* sig,
                                            ID3D12Resource* args, UINT64 argOffset) {
    ID3D12Device5* dev = RealDevice();
    if (!dev || !m_allocator) {
        ProxyLog("[dxr-tier-11-proxy-log] split: no device or allocator (was Reset called?)\n");
        return false;
    }
    groupcount::Capture cap;
    std::string why;
    if (!groupcount::Record(m_real, dev, sig, args, argOffset, &cap, &why)) {
        ProxyLog("[dxr-tier-11-proxy-log] group count capture: %s\n", why.c_str());
        return false;
    }
    // The capture replaced the compute root signature, which clears every root
    // parameter, and the pipeline state. Give the application both back, since
    // it may record more work into this segment before the split closes.
    RestoreComputeAfterCapture();

    // Queued like an indirect DispatchRays: consecutive ones share a segment
    // and one sync.
    Dxr11PendingDispatch pend;
    pend.readback = cap.readback;
    pend.counts = cap.counts;
    pend.rq = m_rqPso;
    pend.rqRef = static_cast<ID3D12PipelineState*>(m_rqPso);
    // Resolved now, from the bindings as recorded; judged at submit.
    if (m_rqPso) pend.rqExact = RayQueryScenes(m_rqPso, &pend.rqScenes);
    pend.bindings = m_bindings;
    pend.bindings.Retain();
    m_openPendings.push_back(pend);
    return true;
}

void Dxr11CommandList::FlushQueuedSplit() {
    if (m_openPendings.empty()) return;
    ID3D12Device5* dev = RealDevice();
    if (!dev || !m_allocator) return;

    if (FAILED(m_real->Close())) {
        ProxyLog("[dxr-tier-11-proxy-log] split: Close failed on the segment\n");
        return;
    }

    Dxr11Segment seg;
    seg.list = m_real;                    // keeps its reference
    seg.pendings.swap(m_openPendings);
    m_segments.push_back(seg);

    // Open the continuation on the SAME allocator. Legal because the previous
    // list is closed; only one list per allocator may be recording at a time.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> next;
    if (FAILED(dev->CreateCommandList(0, m_real->GetType(), m_allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&next)))) {
        ProxyLog("[dxr-tier-11-proxy-log] split: could not open the continuation segment\n");
        return;
    }

    if (m_real10) { m_real10->Release(); m_real10 = nullptr; }
    if (m_real9) { m_real9->Release(); m_real9 = nullptr; }
    if (m_real8) { m_real8->Release(); m_real8 = nullptr; }
    if (m_real7) { m_real7->Release(); m_real7 = nullptr; }
    if (m_real6) { m_real6->Release(); m_real6 = nullptr; }
    if (m_real5) { m_real5->Release(); m_real5 = nullptr; }
    m_real = next.Detach();
    m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList5), (void**)&m_real5);
    m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList6), (void**)&m_real6);
    m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList7), (void**)&m_real7);
    m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList8), (void**)&m_real8);
    m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList9), (void**)&m_real9);
    m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList10), (void**)&m_real10);

    // A fresh list has no state at all, so give the application back everything
    // it had set. The dispatch-only list gets only the compute half, since that
    // is all DispatchRays uses; the continuation needs the graphics half too,
    // because the app carries on recording draws into it.
    m_bindings.Replay(m_real);
    m_gfx.Replay(m_real);
}

bool Dxr11CommandList::SubmitSegmented(ID3D12CommandQueue* queue, SubmitFn submit) {
    ID3D12Device5* dev = RealDevice();
    if (!dev || !queue || !submit) return false;

    if (!m_fence) {
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
            ProxyLog("[dxr-tier-11-proxy-log] split submit: could not create a fence\n");
            return false;
        }
    }

    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt) return false;

    for (size_t i = 0; i < m_segments.size(); ++i) {
        Dxr11Segment& seg = m_segments[i];
        ID3D12CommandList* one[] = { seg.list.Get() };
        submit(queue, 1, one);
        if (seg.pendings.empty()) continue;

        // ONE sync for the whole run of dispatches queued against this segment.
        // Their arguments were all produced by work inside it, so a single wait
        // makes every one of them readable.
        ++m_fenceValue;
        queue->Signal(m_fence.Get(), m_fenceValue);
        if (m_fence->GetCompletedValue() < m_fenceValue) {
            m_fence->SetEventOnCompletion(m_fenceValue, evt);
            WaitForSingleObject(evt, INFINITE);
        }

        for (Dxr11PendingDispatch& pend : seg.pendings) {
            D3D12_DISPATCH_RAYS_DESC desc{};
            UINT groups[3] = { 0, 0, 0 };
            if (pend.rq) {
                // An indirect compute dispatch of a lowered pipeline. Zeros are
                // a legitimately empty dispatch: no group ran, nothing to do.
                groupcount::Capture cap; cap.readback = pend.readback;
                if (!groupcount::Read(cap, groups) ||
                    !groups[0] || !groups[1] || !groups[2]) {
                    dstats::Add(dstats::kIndirectEmpty);
                    static LONG onceEmpty = 0;
                    if (InterlockedCompareExchange(&onceEmpty, 1, 0) == 0)
                        ProxyLog("[dxr-tier-11-proxy-log] split: an indirect compute dispatch "
                                 "read back as %ux%ux%u groups, so nothing ran for it\n",
                                 groups[0], groups[1], groups[2]);
                    continue;
                }
                const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* only =
                    pend.rqExact ? &pend.rqScenes : nullptr;
                if (only && !astrack::AnyRead(*only)) {
                    dstats::Add(dstats::kRefusedUnread);
                    continue;
                }
                std::string why;
                if (astrack::TableWouldBeWrong(pend.rq->CommitsProcedural(), &why, only)) {
                    static LONG onceWrong = 0;
                    if (InterlockedCompareExchange(&onceWrong, 1, 0) == 0)
                        ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch REFUSED: %s. "
                                 "Nothing is drawn for it.\n", why.c_str());
                    continue;
                }
            } else {
            void* p = nullptr;
            D3D12_RANGE readAll{ 0, sizeof(desc) };
            if (SUCCEEDED(pend.readback->Map(0, &readAll, &p)) && p) {
                std::memcpy(&desc, p, sizeof(desc));
                D3D12_RANGE noWrite{ 0, 0 };
                pend.readback->Unmap(0, &noWrite);
            }
            }
            if (!pend.rq && (!desc.Width || !desc.Height || !desc.Depth)) {
                ProxyLog("[dxr-tier-11-proxy-log] split: dimensions read back as %ux%ux%u, "
                         "dispatch skipped\n", desc.Width, desc.Height, desc.Depth);
                continue;
            }

            // Take a dispatch list the GPU has finished with, or make a new one.
            // Nothing blocks here: the pool is checked against the fence value
            // recorded when each list was last submitted.
            Dxr11DispatchList* slot = nullptr;
            const UINT64 done = m_fence->GetCompletedValue();
            for (Dxr11DispatchList& d : m_dispatchPool)
                if (d.fenceValue <= done) { slot = &d; break; }
            if (!slot) {
                Dxr11DispatchList fresh;
                if (FAILED(dev->CreateCommandAllocator(m_real->GetType(), IID_PPV_ARGS(&fresh.alloc)))) {
                    ProxyLog("[dxr-tier-11-proxy-log] split submit: allocator creation failed\n");
                    continue;
                }
                m_dispatchPool.push_back(fresh);
                slot = &m_dispatchPool.back();
            }

            slot->list.Reset();
            if (FAILED(slot->alloc->Reset()) ||
                FAILED(dev->CreateCommandList(0, m_real->GetType(), slot->alloc.Get(),
                                              nullptr, IID_PPV_ARGS(&slot->list)))) {
                ProxyLog("[dxr-tier-11-proxy-log] split submit: dispatch list creation failed\n");
                continue;
            }

            pend.bindings.Replay(slot->list.Get());
            if (pend.rq) dstats::Add(dstats::kIndirect);
            if (pend.rq)
                pend.rq->DispatchAsRays(slot->list.Get(), groups[0], groups[1], groups[2],
                                        astrack::RecordKinds(pend.rqExact ? &pend.rqScenes : nullptr),
                                        slot->list.Get(), pend.rqExact ? &pend.rqScenes : nullptr);
            else
                slot->list->DispatchRays(&desc);
            if (FAILED(slot->list->Close())) {
                gpuhold::Detach(slot->list.Get());   // never submitted
                continue;
            }

            ID3D12CommandList* d[] = { slot->list.Get() };
            submit(queue, 1, d);
            gpuhold::SubmittedOnce(queue, slot->list.Get());

            // Signal WITHOUT waiting. This is only lifetime bookkeeping, so the
            // CPU carries on and the pool reclaims the list later.
            ++m_fenceValue;
            queue->Signal(m_fence.Get(), m_fenceValue);
            slot->fenceValue = m_fenceValue;

            static LONG once = 0, onceRq = 0;
            if (pend.rq) {
                if (InterlockedCompareExchange(&onceRq, 1, 0) == 0)
                    ProxyLog("[dxr-tier-11-proxy-log] ExecuteIndirect(DISPATCH) on a lowered "
                             "RayQuery pipeline -> DispatchRays, %ux%ux%u groups read back "
                             "from the GPU\n", groups[0], groups[1], groups[2]);
            } else if (InterlockedCompareExchange(&once, 1, 0) == 0) {
                ProxyLog("[dxr-tier-11-proxy-log] split dispatch issued: %ux%ux%u, dimensions "
                         "read back from the GPU\n", desc.Width, desc.Height, desc.Depth);
            }
        }
        // One line per segment, whatever its size, so a test can tell "two
        // dispatches behind one sync" apart from "one dispatch, twice". Bounded,
        // because a real application splits on every frame.
        static LONG segLogs = 0;
        if (InterlockedIncrement(&segLogs) <= 8)
            ProxyLog("[dxr-tier-11-proxy-log] split: one sync for %zu dispatch%s\n",
                     seg.pendings.size(), seg.pendings.size() == 1 ? "" : "es");
    }

    // Finally the tail, the part recorded after the last split.
    ID3D12CommandList* tail[] = { m_real };
    submit(queue, 1, tail);

    CloseHandle(evt);
    return true;
}

// --- ID3D12GraphicsCommandList7 to 10 ---------------------------------------
//
// Forwarding, but they exist for the same reason the device's higher
// interfaces do: Unreal asks a command list for ID3D12GraphicsCommandList10,
// and handing over the real list means every command it records afterwards
// bypasses this shim.
//
// Barrier and DispatchGraph go through WorkBarrier because they record work
// that must be ordered after a queued indirect dispatch. The other four are
// state.
//
// NOT replayed across a split, alongside the stream output targets,
// predication, sample positions and shading rate already listed in the header:
// SetProgram, the depth bias, the stencil refs and the strip cut value.
// Nothing tested sets one across a split, and a work graph sharing a recording
// with an indirect ray dispatch would be a remarkable thing to find.

void STDMETHODCALLTYPE Dxr11CommandList::Barrier(UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP* pBarrierGroups) {
    WorkBarrier();
    if (m_real7) m_real7->Barrier(NumBarrierGroups, pBarrierGroups);
}

void STDMETHODCALLTYPE Dxr11CommandList::OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef) {
    if (m_real8) m_real8->OMSetFrontAndBackStencilRef(FrontStencilRef, BackStencilRef);
}

void STDMETHODCALLTYPE Dxr11CommandList::RSSetDepthBias(FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias) {
    if (m_real9) m_real9->RSSetDepthBias(DepthBias, DepthBiasClamp, SlopeScaledDepthBias);
}

void STDMETHODCALLTYPE Dxr11CommandList::IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue) {
    if (m_real9) m_real9->IASetIndexBufferStripCutValue(IBStripCutValue);
}

void STDMETHODCALLTYPE Dxr11CommandList::SetProgram(const D3D12_SET_PROGRAM_DESC* pDesc) {
    if (m_real10) m_real10->SetProgram(pDesc);
}

void STDMETHODCALLTYPE Dxr11CommandList::DispatchGraph(const D3D12_DISPATCH_GRAPH_DESC* pDesc) {
    WorkBarrier();
    if (m_real10) m_real10->DispatchGraph(pDesc);
}

// See the declaration. The pointer handed back by WrapList is our wrapper, so
// this is a static rather than a friend reaching into another object.
void Dxr11CommandList::AdoptAllocator(void* wrappedList, ID3D12CommandAllocator* a) {
    if (!wrappedList || !a) return;
    static_cast<Dxr11CommandList*>(
        static_cast<ID3D12GraphicsCommandList*>(wrappedList))->m_allocator = a;
}

void Dxr11CommandList::AdoptRayQueryPso(void* wrappedList, Dxr11RayQueryPso* rq,
                                        ID3D12PipelineState* initial) {
    if (!wrappedList || !rq) return;
    Dxr11CommandList* self = static_cast<Dxr11CommandList*>(
        static_cast<ID3D12GraphicsCommandList*>(wrappedList));
    self->m_rqPso = rq;
    // The initial state is the bound pipeline state, and a group count capture
    // has to be able to restore it.
    if (initial && !self->m_gfx.pso) { self->m_gfx.pso = initial; initial->AddRef(); }
}