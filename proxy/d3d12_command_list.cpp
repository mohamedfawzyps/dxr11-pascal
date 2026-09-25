// Phase 4 (S1) - ID3D12GraphicsCommandList4 wrapper implementation.
// See d3d12_command_list.h for why the list is wrapped and the queue is hooked.
//
// Everything here is a straight forward for now. The two methods that will grow
// behaviour are ExecuteIndirect and ExecuteBundle, and ExecuteBundle already has
// to unwrap, which is a preview of the work the queue hook does.

#include "d3d12_command_list.h"
#include "geom_index_so.h"

#include <set>
#include <functional>
#include "shim_scene.h"
#include "trace_args.h"
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
    shimscene::DropOwner(this);
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
    shimscene::DropOwner(this);
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
        if (exact && !astrack::Current(scenes)) {
            // What is known about its scene is not the latest build's: GPU-
            // written instances are read back a submission late. Until
            // 0.52.0 an unread scene was refused and a STALE one drawn from
            // the older build's table, wrong after the scene changed. Now
            // the dispatch waits for submit, when that build has run and its
            // instances can be read, exactly as an indirect one does.
            if (!QueueStaleCompute(x, y, z, scenes)) {
                dstats::Add(dstats::kRefusedUnread);
                static LONG onceUnread = 0;
                if (InterlockedCompareExchange(&onceUnread, 1, 0) == 0)
                    ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch REFUSED: the "
                             "scene it traces (0x%llX) is not read from its latest build, and the "
                             "dispatch could not be deferred. Nothing is drawn for it.\n",
                             (unsigned long long)scenes[0]);
            }
            return;
        }
        if (!exact && !astrack::LiveCurrent()) {
            dstats::Add(dstats::kRefusedUnread);
            static LONG onceStale = 0;
            if (InterlockedCompareExchange(&onceStale, 1, 0) == 0)
                ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch REFUSED: the scene "
                         "it traces is not resolved, and a live scene is not read from its "
                         "latest build. Nothing is drawn for it.\n");
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

        // Through the shim's own DispatchRays, so a GeometryIndex() pipeline
        // gets its table; forwarding drew it without one until 0.50.0.
        if (desc.Width && desc.Height && desc.Depth) DispatchRays(&desc);
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
    if (!d || d->Inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) return;
    // GPU-written instances are saved, verbatim, so the shim's copy of this
    // exact build can be made later, by a pipeline created after it; the CPU
    // keeps the ones it can see (CaptureInstances). And while a variant
    // pipeline needs it, the copy itself, built here from the instance
    // buffer the application just built from. See proxy/shim_scene.h.
    shimscene::NoteBuild(d->DestAccelerationStructureData);
    const restrack::Found src = restrack::Find(d->Inputs.InstanceDescs);
    const bool cpuVisible = src.resource &&
        (src.heap == D3D12_HEAP_TYPE_UPLOAD || src.heap == D3D12_HEAP_TYPE_READBACK);
    ID3D12Device5* dev = RealDevice();
    bool replaced = false;
    if (!cpuVisible && dev && d->Inputs.NumDescs) {
        std::string why;
        replaced = true;
        ID3D12Resource* rb = nullptr;
        if (!shimscene::Save(m_real, dev, *d, this, &rb, &why)) {
            static LONG k = 0;
            if (InterlockedIncrement(&k) <= 8)
                ProxyLog("[dxr-tier-11-proxy-log] top-level AS: instances not saved: %s\n",
                         why.c_str());
        }
        // Every build's instances, readable once it has run: read a
        // submission late, or at a split on demand (astrack::BringToBuild).
        if (rb) {
            astrack::NotePendingInstances(d->DestAccelerationStructureData, rb,
                                          d->Inputs.NumDescs, this);
            rb->Release();
        }
    }
    if (shimscene::Active()) {
        std::string why;
        replaced = true;
        if (!dev || !shimscene::Record(m_real, dev, *d, this, &why)) {
            static LONG k = 0;
            if (InterlockedIncrement(&k) <= 8)
                ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): scene copy not built: %s\n",
                         why.empty() ? "no device" : why.c_str());
        }
    }
    if (replaced) RestoreComputeAfterCapture();
}
void STDMETHODCALLTYPE Dxr11CommandList::EmitRaytracingAccelerationStructurePostbuildInfo(const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d, UINT n, const D3D12_GPU_VIRTUAL_ADDRESS* a) { WorkBarrier(); FWD(EmitRaytracingAccelerationStructurePostbuildInfo(d, n, a)); }
// A copy makes a structure the shim did not see built: what it is has to be
// followed, or an instance on a compacted structure is misread (0.53.0).
void STDMETHODCALLTYPE Dxr11CommandList::CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS d, D3D12_GPU_VIRTUAL_ADDRESS s, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE m) {
    WorkBarrier();
    astrack::NoteCopy(d, s, m);
    if (m != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE &&
        m != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS)
        shimscene::NoteCopy(d, s, m == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE ||
                                  m == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
    FWD(CopyRaytracingAccelerationStructure(d, s, m));
}
void STDMETHODCALLTYPE Dxr11CommandList::SetPipelineState1(ID3D12StateObject* s) {
    if (m_bindings.stateObject) m_bindings.stateObject->Release();
    m_bindings.stateObject = s;
    if (s) s->AddRef();
    m_lastBoundStateObject = true;
    FWD(SetPipelineState1(s));
}
namespace {
// The compute root arguments as bound, for gidx.
std::vector<gidx::BoundRoot> RootsOf(const Dxr11Bindings& bb) {
    std::vector<gidx::BoundRoot> roots(Dxr11Bindings::kMaxRootParams);
    for (UINT i = 0; i < Dxr11Bindings::kMaxRootParams; ++i) {
        const auto& r = bb.roots[i];
        if (r.kind == Dxr11RootParam::SRV) roots[i].srv = r.address;
        if (r.kind == Dxr11RootParam::CBV) roots[i].cbv = r.address;
        if (r.kind == Dxr11RootParam::Table) roots[i].table = r.table.ptr;
        if (r.kind == Dxr11RootParam::Constants) {
            roots[i].constants = r.constants.data();
            roots[i].numConstants = (UINT)r.constants.size();
        }
    }
    return roots;
}
}  // namespace

bool Dxr11CommandList::ResolveBoundScenes(const gidx::Scenes& sc,
                                          std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* out,
                                          std::string* why, const Dxr11Bindings* with,
                                          const gidx::LocalRecord* local, bool* needsLocal,
                                          std::vector<std::pair<UINT, D3D12_GPU_VIRTUAL_ADDRESS>>* keys) {
    const Dxr11Bindings& bb = with ? *with : m_bindings;
    return gidx::ResolveScenes(sc, bb.rootSig, RootsOf(bb), bb.heaps.data(), (UINT)bb.heaps.size(),
                               out, why, local, needsLocal, keys);
}

namespace {
// How much of a raygen record is read: all of it, up to the largest local
// root arguments a record can carry, in whole words.
UINT RaygenRecordBytes(const D3D12_DISPATCH_RAYS_DESC& d) {
    return (UINT)((std::min)(d.RayGenerationShaderRecord.SizeInBytes, (UINT64)4096) & ~3ull);
}
bool ReadBackBytes(ID3D12Resource* rb, UINT bytes, std::vector<uint8_t>* out) {
    void* p = nullptr;
    D3D12_RANGE all{ 0, bytes };
    if (!rb || FAILED(rb->Map(0, &all, &p)) || !p) return false;
    out->assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + bytes);
    D3D12_RANGE none{ 0, 0 };
    rb->Unmap(0, &none);
    return true;
}
}  // namespace

namespace {
// Range k of a dispatch's records: 0 the raygen record, 1 the miss table, 2
// the hit group table. A table is read whole, up to what one copy can move.
void RecordRange(const D3D12_DISPATCH_RAYS_DESC& d, int k, D3D12_GPU_VIRTUAL_ADDRESS* at, UINT* bytes) {
    if (k == 0) { *at = d.RayGenerationShaderRecord.StartAddress; *bytes = RaygenRecordBytes(d); return; }
    const auto& t = k == 1 ? d.MissShaderTable : d.HitGroupTable;
    *at = t.StartAddress;
    *bytes = (UINT)((std::min)(t.SizeInBytes, (UINT64)(16u << 20)) & ~3ull);
}
// 1 read from CPU-visible memory, 2 not (GPU memory, or not a tracked buffer).
int ReadRange(D3D12_GPU_VIRTUAL_ADDRESS at, UINT bytes, std::vector<uint8_t>* out) {
    const restrack::Found f = restrack::Find(at);
    if (!f.resource || (f.heap != D3D12_HEAP_TYPE_UPLOAD && f.heap != D3D12_HEAP_TYPE_READBACK))
        return 2;
    const UINT64 width = f.resource->GetDesc().Width;
    if (f.offset >= width) return 2;
    if (f.offset + bytes > width) bytes = (UINT)((width - f.offset) & ~3ull);
    uint8_t* p = nullptr;
    D3D12_RANGE rr{ (SIZE_T)f.offset, (SIZE_T)(f.offset + bytes) };
    if (FAILED(f.resource->Map(0, &rr, reinterpret_cast<void**>(&p))) || !p) return 2;
    out->assign(p + f.offset, p + f.offset + bytes);
    D3D12_RANGE none{ 0, 0 };
    f.resource->Unmap(0, &none);
    return 1;
}
}  // namespace

namespace {
// The TraceRay argument pairs a GeometryIndex() dispatch can use: the
// constants its arguments are computed from read as `b` binds them, and for
// a raygen's calls from its record when the CPU can read it (0.57.0).
std::vector<std::pair<UINT, UINT>> DispatchPairs(gidx::Info& gi, const Dxr11Bindings& b,
                                                 const D3D12_DISPATCH_RAYS_DESC& d) {
    if (targs::Literal(gi.args)) return gi.traceArgs;
    std::vector<uint8_t> rec;
    gidx::LocalRecord lr;
    const gidx::LocalRecord* raygen = nullptr;
    UINT recursion = 0;
    std::string why;
    if (RaygenRecordBytes(d) >= D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES &&
        gidx::PrepareRecordLocals(gi, b.stateObject, &recursion, &why) &&
        ReadRange(d.RayGenerationShaderRecord.StartAddress, RaygenRecordBytes(d), &rec) == 1 &&
        rec.size() >= D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES) {
        const gidx::RecordLocal* x = gidx::RecordLocalOf(gi, rec.data());
        if (x && x->why.empty() && x->desc) {
            lr.desc = x->desc;
            lr.record = rec.data();
            lr.size = (UINT)rec.size();
            raygen = &lr;
        }
    }
    return gidx::TracePairs(gi, b.rootSig, RootsOf(b), raygen);
}
}  // namespace

int Dxr11CommandList::ReadRecords(const D3D12_DISPATCH_RAYS_DESC& d, bool all,
                                  std::vector<uint8_t> out[3], std::string* why) {
    if (RaygenRecordBytes(d) < D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES) {
        *why = "the raygen record is shorter than a shader identifier";
        return 0;
    }
    int r = 1;
    for (int k = 0; k < (all ? 3 : 1); ++k) {
        D3D12_GPU_VIRTUAL_ADDRESS at = 0;
        UINT bytes = 0;
        RecordRange(d, k, &at, &bytes);
        if (bytes && ReadRange(at, bytes, &out[k]) == 2) r = 2;
    }
    return r;
}

void Dxr11CommandList::GlobalSel(gidx::Info& gi, const Dxr11Bindings& b, gidx::SceneSel* sel) {
    *sel = gidx::SceneSel{};
    sel->valid = true;
    const auto slots = gidx::SceneSlots(gi.scenes);
    sel->global.assign(slots.size(), {});
    sel->perRecord.assign(slots.size(), false);
    for (size_t j = 0; j < slots.size(); ++j) {
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> s;
        std::vector<std::pair<UINT, D3D12_GPU_VIRTUAL_ADDRESS>> keys;
        std::string w;
        bool needs = false;
        if (ResolveBoundScenes(slots[j], &s, &w, &b, nullptr, &needs, &keys)) {
            // A scene picked by key: every scene each key names (0.60.0;
            // until then refused).
            if (gidx::KeyedSlot(slots[j])) {
                sel->global[j].keyed = true;
                sel->global[j].keys = keys;
            } else if (s.size() == 1) {
                sel->global[j].scene = s[0];
            } else {
                sel->valid = false;
                sel->why = "a scene slot not picked by key resolves to several scenes";
                return;
            }
            continue;
        }
        if (needs) { sel->perRecord[j] = true; continue; }
        sel->valid = false;
        sel->why = w;
        return;
    }
    const std::vector<std::vector<gidx::SlotScenes>> none[3];
    gidx::BuildSel(sel->global, sel->perRecord, none, sel);
}

bool Dxr11CommandList::RecordScenes(gidx::Info& gi, const Dxr11Bindings& b,
                                    const D3D12_DISPATCH_RAYS_DESC& d, bool all,
                                    const std::vector<uint8_t> rec[3],
                                    std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* srvs, std::string* why,
                                    gidx::SceneSel* sel) {
    static const uint8_t kNull[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = {};
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> scenes;
    std::set<std::string> seen;
    // One record: its signature, and the scenes it names. At a recursion
    // depth above 1 every record is lenient: a register one shader does not
    // declare is another shader's.
    auto one = [&](const uint8_t* r, UINT size, bool lenient) -> bool {
        if (size < D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES) {
            *why = "a record shorter than a shader identifier";
            return false;
        }
        if (!std::memcmp(r, kNull, sizeof(kNull))) return true;   // a null record runs nothing
        if (!seen.insert(std::string(reinterpret_cast<const char*>(r), size)).second) return true;
        const gidx::RecordLocal* rl = gidx::RecordLocalOf(gi, r);
        if (!rl) { *why = "a record whose identifier is not one of the pipeline's"; return false; }
        if (!rl->why.empty()) { *why = rl->why; return false; }
        gidx::LocalRecord lr;
        lr.desc = rl->desc;
        lr.record = r;
        lr.size = size;
        lr.lenient = lenient;
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> s;
        if (!ResolveBoundScenes(gi.scenes, &s, why, &b, &lr, nullptr)) return false;
        for (auto a : s)
            if (std::find(scenes.begin(), scenes.end(), a) == scenes.end()) scenes.push_back(a);
        return true;
    };
    if (!one(rec[0].data(), (UINT)rec[0].size(), all)) return false;
    for (int k = 1; all && k < 3; ++k) {
        const UINT64 size = rec[k].size();
        UINT64 stride = (k == 1 ? d.MissShaderTable : d.HitGroupTable).StrideInBytes;
        if (!stride || stride > size) stride = size;
        for (UINT64 off = 0; off + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES <= size; off += stride)
            if (!one(rec[k].data() + off, (UINT)(std::min)(stride, size - off), true)) return false;
    }
    if (scenes.empty()) { *why = "no record names a scene"; return false; }
    *srvs = scenes;
    // Per record, the scene of every slot the global root signature leaves
    // to the records: a record carries its own (0.59.0), and with a keyed
    // slot the scene of each key (0.60.0).
    if (sel && sel->valid) {
        const auto slots = gidx::SceneSlots(gi.scenes);
        const size_t S = slots.size();
        bool any = false;
        for (size_t j = 0; j < S && j < sel->perRecord.size(); ++j) any = any || sel->perRecord[j];
        std::vector<std::vector<gidx::SlotScenes>> rows[3];
        for (int k = 0; any && k < (all ? 3 : 1); ++k) {
            const UINT64 size = rec[k].size();
            UINT64 stride = k == 0 ? size : (k == 1 ? d.MissShaderTable : d.HitGroupTable).StrideInBytes;
            if (!stride || stride > size) stride = size;
            for (UINT64 off = 0; stride && off + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES <= size; off += stride) {
                const uint8_t* r = rec[k].data() + off;
                std::vector<gidx::SlotScenes> row(S);
                const gidx::RecordLocal* rl =
                    std::memcmp(r, kNull, sizeof(kNull)) ? gidx::RecordLocalOf(gi, r) : nullptr;
                if (rl && rl->why.empty() && rl->desc) {
                    gidx::LocalRecord lr;
                    lr.desc = rl->desc;
                    lr.record = r;
                    lr.size = (UINT)(std::min)(stride, size - off);
                    lr.lenient = true;
                    for (size_t j = 0; j < S; ++j) {
                        if (!sel->perRecord[j]) continue;
                        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> s;
                        std::vector<std::pair<UINT, D3D12_GPU_VIRTUAL_ADDRESS>> keys;
                        std::string w;
                        if (!ResolveBoundScenes(slots[j], &s, &w, &b, &lr, nullptr, &keys)) {
                            sel->valid = false;
                            sel->why = w;
                            return true;
                        }
                        if (gidx::KeyedSlot(slots[j])) {
                            row[j].keyed = true;
                            row[j].keys = keys;
                        } else if (s.size() == 1) {
                            row[j].scene = s[0];
                        } else if (s.size() > 1) {
                            sel->valid = false;
                            sel->why = "a scene slot not picked by key resolves to several scenes";
                            return true;
                        }
                    }
                }
                rows[k].push_back(row);
            }
        }
        if (any) gidx::BuildSel(sel->global, sel->perRecord, rows, sel);
    }
    return true;
}

bool Dxr11CommandList::QueueLocalRays(gidx::Info& gi, const D3D12_DISPATCH_RAYS_DESC& d) {
    ID3D12Device5* dev = RealDevice();
    UINT recursion = 0;
    std::string why;
    if (!dev || !m_allocator || RaygenRecordBytes(d) < D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES ||
        !gidx::PrepareRecordLocals(gi, m_bindings.stateObject, &recursion, &why))
        return false;
    Dxr11PendingDispatch pend;
    // A copy of each range in GPU memory, recorded before the dispatch; the
    // CPU-visible ones are read at submit.
    for (int k = 0; k < (recursion != 1 ? 3 : 1); ++k) {
        D3D12_GPU_VIRTUAL_ADDRESS at = 0;
        UINT bytes = 0;
        RecordRange(d, k, &at, &bytes);
        std::vector<uint8_t> probe;
        if (!bytes || ReadRange(at, bytes, &probe) == 1) continue;
        if (!shimscene::CopyBytes(m_real, dev, at, bytes, &pend.giRecord[k], &pend.giRecordScratch[k], &why)) {
            ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): a shader record range could not be "
                     "copied (%s)\n", why.c_str());
            RestoreComputeAfterCapture();
            return false;
        }
    }
    RestoreComputeAfterCapture();
    pend.giScenesSet = true;
    pend.giLocal = true;
    pend.giSerial = astrack::SerialNow();
    pend.giDirect = true;
    pend.giDesc = d;
    pend.bindings = m_bindings;
    pend.bindings.Retain();
    m_openPendings.push_back(pend);
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex() dispatch deferred to submit: its scene "
                 "is in shader records in GPU memory\n");
    return true;
}

bool Dxr11CommandList::FillRecordsNow(ID3D12CommandQueue* queue, SubmitFn submit, HANDLE evt,
                                      const D3D12_DISPATCH_RAYS_DESC& d, bool all,
                                      std::vector<uint8_t> rec[3], std::string* why) {
    ID3D12Device5* dev = RealDevice();
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> al;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cl;
    Microsoft::WRL::ComPtr<ID3D12Resource> rb[3], scratch[3];
    UINT sizes[3] = { 0, 0, 0 };
    bool any = false;
    for (int k = 0; k < (all ? 3 : 1); ++k) {
        D3D12_GPU_VIRTUAL_ADDRESS at = 0;
        UINT bytes = 0;
        RecordRange(d, k, &at, &bytes);
        if (!bytes || !rec[k].empty() || ReadRange(at, bytes, &rec[k]) == 1) continue;
        if (!cl && (!dev || FAILED(dev->CreateCommandAllocator(m_real->GetType(), IID_PPV_ARGS(&al))) ||
                    FAILED(dev->CreateCommandList(0, m_real->GetType(), al.Get(), nullptr,
                                                  IID_PPV_ARGS(&cl))))) {
            *why = "could not create a list to read the shader records";
            return false;
        }
        if (!shimscene::CopyBytes(cl.Get(), dev, at, bytes, &rb[k], &scratch[k], why)) return false;
        sizes[k] = bytes;
        any = true;
    }
    if (!any) return true;
    if (FAILED(cl->Close())) { *why = "could not close the record copy"; return false; }
    ID3D12CommandList* one[] = { cl.Get() };
    submit(queue, 1, one);
    ++m_fenceValue;
    queue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, evt);
        WaitForSingleObject(evt, INFINITE);
    }
    for (int k = 0; k < 3; ++k)
        if (sizes[k] && !ReadBackBytes(rb[k].Get(), sizes[k], &rec[k])) {
            *why = "could not read the record copy";
            return false;
        }
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): shader records in GPU memory with "
                 "GPU-written arguments, read at submit with one extra wait\n");
    return true;
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
// Records one GeometryIndex() dispatch into `cl`: the shim's table, or the
// variant in the shim's own layout, or nothing, logged. `b` are the bindings
// the application left, `restore` gives them back after the shim's own
// passes. Shared by DispatchRays and by an indirect DispatchRays, CPU-visible
// or issued at submit (0.50.0; until then both drew against the
// application's table, without the geometry index).
static void DispatchGeometryIndex(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev,
                                  gidx::Info& gi, ID3D12StateObject* so,
                                  const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& srvs, bool exact,
                                  const gidx::SceneSel& sel,
                                  const std::vector<std::pair<UINT, UINT>>& pairs,
                                  const void* owner, const D3D12_DISPATCH_RAYS_DESC& d,
                                  const std::function<void()>& restore) {
    D3D12_DISPATCH_RAYS_DESC mine = d;
    std::string why;
    bool shared = false;
    if (!dev || !gidx::RecordTable(cl, dev, gi, pairs, d.HitGroupTable, &mine.HitGroupTable,
                                   srvs, exact, owner, &shared, &why)) {
        // A record several geometries reach: the shim's own layout, through
        // the variant pipeline and the shim's copy of the scene.
        std::string vwhy;
        std::shared_ptr<gidx::Variant> var;
        if (dev && shared && gidx::EnsureVariant(dev, gi, so, sel.need, &var, &vwhy) &&
            gidx::RecordVariant(cl, dev, gi, *var, pairs, d, &mine, srvs, exact, sel, owner, &vwhy)) {
            restore();
            cl->SetPipelineState1(var->so.Get());
            cl->DispatchRays(&mine);
            cl->SetPipelineState1(so);
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
        restore();
        return;
    }
    restore();
    static LONG first = 0;
    if (InterlockedCompareExchange(&first, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): first dispatch against the shim's "
                 "table, %llu records at stride %llu (the application's: stride %llu)\n",
                 (unsigned long long)(mine.HitGroupTable.SizeInBytes /
                                      mine.HitGroupTable.StrideInBytes),
                 (unsigned long long)mine.HitGroupTable.StrideInBytes,
                 (unsigned long long)d.HitGroupTable.StrideInBytes);
    cl->DispatchRays(&mine);
}

// The scene a GeometryIndex() dispatch traces: resolved through the bound
// root signature when every TraceRay's scene can be, root SRV, descriptor
// table or heap index. Otherwise every live scene, which can refuse a layout
// that was fine but never draws wrong. (Until 0.48.0 a bound root SRV that
// was a known scene counted as THE scene; wrong when the shader traces
// another one, a heap-indexed one say, beside it.)
bool Dxr11CommandList::GeometryIndexScenes(gidx::Info& gi,
                                           std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* srvs,
                                           const D3D12_DISPATCH_RAYS_DESC* d, bool* defer,
                                           gidx::SceneSel* sel) {
    std::string swhy;
    bool needsLocal = false;
    if (defer) *defer = false;
    if (sel) GlobalSel(gi, m_bindings, sel);
    if (ResolveBoundScenes(gi.scenes, srvs, &swhy, nullptr, nullptr, &needsLocal)) return true;
    // Not in the global root signature: the records' local ones, read now
    // when they are in CPU-visible memory, else at submit. The raygen's
    // (0.55.0), and at a recursion depth above 1 every miss and hit group
    // record too, whose shaders can trace (0.56.0).
    UINT recursion = 0;
    if (needsLocal && gidx::PrepareRecordLocals(gi, m_bindings.stateObject, &recursion, &swhy)) {
        const bool all = recursion != 1;
        std::vector<uint8_t> rec[3];
        const int k = d ? ReadRecords(*d, all, rec, &swhy) : 2;
        if (k == 1 && RecordScenes(gi, m_bindings, *d, all, rec, srvs, &swhy, sel)) return true;
        if (k == 2 && defer) {
            *defer = true;
            srvs->clear();
            return false;
        }
    }
    srvs->clear();
    static LONG n = 0;
    if (InterlockedIncrement(&n) <= 4)
        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): the scene this dispatch traces is not "
                 "resolved (%s); judged over every live scene\n", swhy.c_str());
    return false;
}

void STDMETHODCALLTYPE Dxr11CommandList::DispatchRays(const D3D12_DISPATCH_RAYS_DESC* d) {
    WorkBarrier();
    std::shared_ptr<gidx::Info> gi = d ? gidx::Get(m_bindings.stateObject) : nullptr;
    if (!gi) { FWD(DispatchRays(d)); return; }
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> srvs;
    bool defer = false;
    gidx::SceneSel sel;
    const bool exact = GeometryIndexScenes(*gi, &srvs, d, &defer, &sel);
    // Its scene in its raygen record, in GPU memory: read at submit (0.55.0).
    if (defer && QueueLocalRays(*gi, *d)) return;
    // Its scene not read from its latest build: at submit that build has run
    // and is read exactly, as a RayQuery dispatch has been since 0.52.0.
    // Until 0.54.0 the variant drew it from the GPU copy without knowing
    // which bottom-level structures the instances point at, so one of
    // unknown geometry (deserialized) spilled into the next instance's
    // records, silently.
    if (exact && !astrack::Current(srvs) && QueueStaleRays(*d, srvs, sel)) return;
    DispatchGeometryIndex(m_real, RealDevice(), *gi, m_bindings.stateObject, srvs, exact, sel,
                          DispatchPairs(*gi, m_bindings, *d), this, *d,
                          [this] { RestoreComputeAfterCapture(); });
}

bool Dxr11CommandList::QueueStaleRays(const D3D12_DISPATCH_RAYS_DESC& d,
                                      const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& scenes,
                                      const gidx::SceneSel& sel) {
    if (!RealDevice() || !m_allocator || scenes.empty()) return false;
    Dxr11PendingDispatch pend;
    pend.giScenesSet = true;
    pend.giExact = true;
    pend.giScenes = scenes;
    pend.giSel = sel;
    for (auto a : scenes) {
        pend.giBuilds.push_back(astrack::LatestBuild(a));
        if (!pend.giBuilds.back()) return false;   // never seen being built
    }
    pend.giDirect = true;
    pend.giDesc = d;
    pend.bindings = m_bindings;
    pend.bindings.Retain();
    m_openPendings.push_back(pend);
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex() dispatch deferred to submit: its "
                 "scene's latest build is not read yet\n");
    return true;
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
    if (!in.NumDescs) {
        astrack::NoteEmpty(desc->DestAccelerationStructureData);
        return;
    }
    if (!in.InstanceDescs) return;

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

    // Produced on the GPU: every build's are read back by the save pass
    // right after it (BuildRaytracingAccelerationStructure, 0.52.0). Until
    // then they were copied out here every 8 builds, so what was known could
    // be an older build's.
    if (!cheap) return;
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
    if (std::shared_ptr<gidx::Info> gi = gidx::Get(m_bindings.stateObject)) {
        pend.giScenesSet = true;
        pend.giExact = GeometryIndexScenes(*gi, &pend.giScenes, nullptr, &pend.giLocal, &pend.giSel);
        pend.giSerial = astrack::SerialNow();
        for (auto a : pend.giScenes) pend.giBuilds.push_back(astrack::LatestBuild(a));
    }
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
    // Resolved now, from the bindings as recorded; judged at submit, from the
    // builds that are the latest now.
    if (m_rqPso) pend.rqExact = RayQueryScenes(m_rqPso, &pend.rqScenes);
    for (auto a : pend.rqScenes) pend.rqBuilds.push_back(astrack::LatestBuild(a));
    pend.bindings = m_bindings;
    pend.bindings.Retain();
    m_openPendings.push_back(pend);
    return true;
}

// A direct Dispatch of a lowered pipeline whose scene is not read from its
// latest build: queued like an indirect one, so at submit that build has run
// and its instances can be read (0.52.0).
bool Dxr11CommandList::QueueStaleCompute(UINT x, UINT y, UINT z,
                                         const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& scenes) {
    if (!RealDevice() || !m_allocator || !m_rqPso) return false;
    Dxr11PendingDispatch pend;
    pend.rq = m_rqPso;
    pend.rqRef = static_cast<ID3D12PipelineState*>(m_rqPso);
    pend.rqExact = true;
    pend.rqScenes = scenes;
    for (auto a : scenes) pend.rqBuilds.push_back(astrack::LatestBuild(a));
    for (auto b : pend.rqBuilds)
        if (!b) return false;   // never seen being built
    pend.rqDirect = true;
    pend.rqGroups[0] = x; pend.rqGroups[1] = y; pend.rqGroups[2] = z;
    pend.bindings = m_bindings;
    pend.bindings.Retain();
    m_openPendings.push_back(pend);
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch deferred to submit: its "
                 "scene's latest build is not read yet\n");
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
                if (pend.rqDirect) std::memcpy(groups, pend.rqGroups, sizeof(groups));
                if ((!pend.rqDirect && !groupcount::Read(cap, groups)) ||
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
                // The segment has run, so the build each scene had when the
                // dispatch was recorded has too: its table comes from exactly
                // those instances.
                bool haveBuilds = true;
                for (size_t k = 0; only && k < pend.rqScenes.size(); ++k)
                    haveBuilds = haveBuilds && k < pend.rqBuilds.size() &&
                                 astrack::BringToBuild(pend.rqScenes[k], pend.rqBuilds[k], this);
                if (!haveBuilds || (!only && !astrack::LiveCurrent())) {
                    dstats::Add(dstats::kRefusedUnread);
                    static LONG onceNoBuild = 0;
                    if (InterlockedCompareExchange(&onceNoBuild, 1, 0) == 0)
                        ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch REFUSED at "
                                 "submit: the instances of the build its scene had could not be "
                                 "read. Nothing is drawn for it.\n");
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
            } else if (pend.giDirect) {
                desc = pend.giDesc;
            } else {
            void* p = nullptr;
            D3D12_RANGE readAll{ 0, sizeof(desc) };
            if (SUCCEEDED(pend.readback->Map(0, &readAll, &p)) && p) {
                std::memcpy(&desc, p, sizeof(desc));
                D3D12_RANGE noWrite{ 0, 0 };
                pend.readback->Unmap(0, &noWrite);
            }
            }
            // Its scene in the raygen record (0.55.0): the segment has run, so
            // the copy recorded before it, or the record itself, can be read.
            if (!pend.rq && pend.giScenesSet && pend.giLocal) {
                std::shared_ptr<gidx::Info> gi = gidx::Get(pend.bindings.stateObject);
                std::vector<uint8_t> rec[3];
                std::string lw;
                UINT recursion = 0;
                bool have = gi && gidx::PrepareRecordLocals(*gi, pend.bindings.stateObject, &recursion, &lw);
                const bool all = recursion != 1;
                for (int k = 0; have && k < 3; ++k)
                    if (pend.giRecord[k])
                        have = ReadBackBytes(pend.giRecord[k].Get(),
                                             (UINT)pend.giRecord[k]->GetDesc().Width, &rec[k]);
                have = have && FillRecordsNow(queue, submit, evt, desc, all, rec, &lw);
                std::vector<D3D12_GPU_VIRTUAL_ADDRESS> srvs;
                if (have) GlobalSel(*gi, pend.bindings, &pend.giSel);
                if (have && RecordScenes(*gi, pend.bindings, desc, all, rec, &srvs, &lw, &pend.giSel)) {
                    bool later = false;
                    pend.giScenes = srvs;
                    pend.giExact = true;
                    pend.giBuilds.clear();
                    for (auto a : srvs) {
                        const UINT64 b = astrack::LatestBuild(a);
                        later = later || b > pend.giSerial;
                        pend.giBuilds.push_back(b);
                    }
                    if (later) {
                        static LONG n = 0;
                        if (InterlockedIncrement(&n) <= 16)
                            ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex() dispatch NOT DRAWN at "
                                     "submit: its scene was built again after it was recorded, "
                                     "before it was submitted\n");
                        continue;
                    }
                } else {
                    pend.giScenes.clear();
                    pend.giExact = false;
                    static LONG n = 0;
                    if (InterlockedIncrement(&n) <= 4)
                        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): the scene in the shader "
                                 "records is not resolved at submit (%s); judged over every live "
                                 "scene\n", lw.empty() ? "the record could not be read" : lw.c_str());
                }
            }
            // A GeometryIndex() dispatch: the segment has run, so the build
            // its scene had when it was recorded has too, and is read now.
            // A build recorded after it, not yet run, would make the read
            // not the latest: named, not drawn from the wrong one.
            if (!pend.rq && pend.giScenesSet && pend.giExact) {
                bool ok = pend.giBuilds.size() == pend.giScenes.size();
                for (size_t k = 0; ok && k < pend.giScenes.size(); ++k)
                    ok = pend.giBuilds[k] &&
                         astrack::BringToBuild(pend.giScenes[k], pend.giBuilds[k], this);
                const bool current = ok && astrack::Current(pend.giScenes);
                if (!current) {
                    static LONG n = 0;
                    if (InterlockedIncrement(&n) <= 16)
                        ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex() dispatch NOT DRAWN at "
                                 "submit: %s\n", ok ? "its scene was built again after it was "
                                 "recorded, before it was submitted"
                                 : "the instances of the build its scene had could not be read");
                    continue;
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
            if (pend.rq && !pend.rqDirect) dstats::Add(dstats::kIndirect);
            if (pend.rq)
                pend.rq->DispatchAsRays(slot->list.Get(), groups[0], groups[1], groups[2],
                                        astrack::RecordKinds(pend.rqExact ? &pend.rqScenes : nullptr),
                                        slot->list.Get(), pend.rqExact ? &pend.rqScenes : nullptr);
            else if (std::shared_ptr<gidx::Info> gi =
                         pend.giScenesSet ? gidx::Get(pend.bindings.stateObject) : nullptr) {
                Microsoft::WRL::ComPtr<ID3D12Device5> dev5;
                slot->list->GetDevice(IID_PPV_ARGS(&dev5));
                ID3D12GraphicsCommandList4* sl = slot->list.Get();
                const Dxr11Bindings& pb = pend.bindings;
                DispatchGeometryIndex(sl, dev5.Get(), *gi, pb.stateObject, pend.giScenes,
                                      pend.giExact, pend.giSel, DispatchPairs(*gi, pb, desc), sl, desc,
                                      [sl, &pb] { pb.Replay(sl); });
            } else {
                slot->list->DispatchRays(&desc);
            }
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