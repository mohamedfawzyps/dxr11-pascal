// Phase 4 (S1) - ID3D12GraphicsCommandList4 wrapper implementation.
// See d3d12_command_list.h for why the list is wrapped and the queue is hooked.
//
// Everything here is a straight forward for now. The two methods that will grow
// behaviour are ExecuteIndirect and ExecuteBundle, and ExecuteBundle already has
// to unwrap, which is a preview of the work the queue hook does.

#include "d3d12_command_list.h"
#include "proxy_log.h"

#include <windows.h>

#define FWD(call) m_real->call

// {A6B41C7E-2E5D-4C3B-9F80-1D4E6A0F2B91}
const GUID IID_Dxr11CommandList =
    { 0xa6b41c7e, 0x2e5d, 0x4c3b, { 0x9f, 0x80, 0x1d, 0x4e, 0x6a, 0x0f, 0x2b, 0x91 } };

Dxr11CommandList::Dxr11CommandList(ID3D12GraphicsCommandList4* real)
    : m_real(real), m_real5(nullptr), m_real6(nullptr), m_refs(1) {
    if (m_real) {
        m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList5), (void**)&m_real5);
        m_real->QueryInterface(__uuidof(ID3D12GraphicsCommandList6), (void**)&m_real6);
    }
}

Dxr11CommandList::~Dxr11CommandList() {
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
        (riid == __uuidof(ID3D12GraphicsCommandList6) && m_real6)) {
        AddRef();
        *ppvObject = static_cast<ID3D12GraphicsCommandList6*>(this);
        return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList5) ||
        riid == __uuidof(ID3D12GraphicsCommandList6))
        return E_NOINTERFACE;

    // Anything above GraphicsCommandList6 goes out unwrapped and bypasses our
    // ExecuteIndirect. Log it: this is the same hole the device wrapper has for
    // Device8 and above, and the log is how we find out an app needs it.
    HRESULT hr = m_real->QueryInterface(riid, ppvObject);
    if (SUCCEEDED(hr))
        ProxyLog("[dxr11-proxy] command list QI PASSED THROUGH UNWRAPPED: %s\n",
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

HRESULT STDMETHODCALLTYPE Dxr11CommandList::Close() { return FWD(Close()); }
HRESULT STDMETHODCALLTYPE Dxr11CommandList::Reset(ID3D12CommandAllocator* a, ID3D12PipelineState* p) { return FWD(Reset(a, p)); }
void STDMETHODCALLTYPE Dxr11CommandList::ClearState(ID3D12PipelineState* p) { FWD(ClearState(p)); }
void STDMETHODCALLTYPE Dxr11CommandList::DrawInstanced(UINT a, UINT b, UINT c, UINT d) { FWD(DrawInstanced(a, b, c, d)); }
void STDMETHODCALLTYPE Dxr11CommandList::DrawIndexedInstanced(UINT a, UINT b, UINT c, INT d, UINT e) { FWD(DrawIndexedInstanced(a, b, c, d, e)); }
void STDMETHODCALLTYPE Dxr11CommandList::Dispatch(UINT x, UINT y, UINT z) { FWD(Dispatch(x, y, z)); }
void STDMETHODCALLTYPE Dxr11CommandList::CopyBufferRegion(ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT64 n) { FWD(CopyBufferRegion(d, dof, s, sof, n)); }
void STDMETHODCALLTYPE Dxr11CommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* b) { FWD(CopyTextureRegion(d, x, y, z, s, b)); }
void STDMETHODCALLTYPE Dxr11CommandList::CopyResource(ID3D12Resource* d, ID3D12Resource* s) { FWD(CopyResource(d, s)); }
void STDMETHODCALLTYPE Dxr11CommandList::CopyTiles(ID3D12Resource* r, const D3D12_TILED_RESOURCE_COORDINATE* c, const D3D12_TILE_REGION_SIZE* sz, ID3D12Resource* b, UINT64 off, D3D12_TILE_COPY_FLAGS f) { FWD(CopyTiles(r, c, sz, b, off, f)); }
void STDMETHODCALLTYPE Dxr11CommandList::ResolveSubresource(ID3D12Resource* d, UINT ds, ID3D12Resource* s, UINT ss, DXGI_FORMAT f) { FWD(ResolveSubresource(d, ds, s, ss, f)); }
void STDMETHODCALLTYPE Dxr11CommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t) { FWD(IASetPrimitiveTopology(t)); }
void STDMETHODCALLTYPE Dxr11CommandList::RSSetViewports(UINT n, const D3D12_VIEWPORT* v) { FWD(RSSetViewports(n, v)); }
void STDMETHODCALLTYPE Dxr11CommandList::RSSetScissorRects(UINT n, const D3D12_RECT* r) { FWD(RSSetScissorRects(n, r)); }
void STDMETHODCALLTYPE Dxr11CommandList::OMSetBlendFactor(const FLOAT f[4]) { FWD(OMSetBlendFactor(f)); }
void STDMETHODCALLTYPE Dxr11CommandList::OMSetStencilRef(UINT s) { FWD(OMSetStencilRef(s)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetPipelineState(ID3D12PipelineState* p) { FWD(SetPipelineState(p)); }
void STDMETHODCALLTYPE Dxr11CommandList::ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b) { FWD(ResourceBarrier(n, b)); }

// A bundle is created through CreateCommandList too, so it arrives wrapped.
// Unwrap before the runtime sees it. Same problem the queue hook solves, in
// miniature.
void STDMETHODCALLTYPE Dxr11CommandList::ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) {
    ID3D12CommandList* real = Dxr11CommandList::Unwrap(pCommandList);
    FWD(ExecuteBundle(real ? static_cast<ID3D12GraphicsCommandList*>(real) : pCommandList));
}

void STDMETHODCALLTYPE Dxr11CommandList::SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* h) { FWD(SetDescriptorHeaps(n, h)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRootSignature(ID3D12RootSignature* r) { FWD(SetComputeRootSignature(r)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRootSignature(ID3D12RootSignature* r) { FWD(SetGraphicsRootSignature(r)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { FWD(SetComputeRootDescriptorTable(i, h)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { FWD(SetGraphicsRootDescriptorTable(i, h)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRoot32BitConstant(UINT i, UINT v, UINT o) { FWD(SetComputeRoot32BitConstant(i, v, o)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRoot32BitConstant(UINT i, UINT v, UINT o) { FWD(SetGraphicsRoot32BitConstant(i, v, o)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRoot32BitConstants(UINT i, UINT n, const void* d, UINT o) { FWD(SetComputeRoot32BitConstants(i, n, d, o)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRoot32BitConstants(UINT i, UINT n, const void* d, UINT o) { FWD(SetGraphicsRoot32BitConstants(i, n, d, o)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { FWD(SetComputeRootConstantBufferView(i, a)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { FWD(SetGraphicsRootConstantBufferView(i, a)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { FWD(SetComputeRootShaderResourceView(i, a)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { FWD(SetGraphicsRootShaderResourceView(i, a)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetComputeRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { FWD(SetComputeRootUnorderedAccessView(i, a)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetGraphicsRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { FWD(SetGraphicsRootUnorderedAccessView(i, a)); }
void STDMETHODCALLTYPE Dxr11CommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) { FWD(IASetIndexBuffer(v)); }
void STDMETHODCALLTYPE Dxr11CommandList::IASetVertexBuffers(UINT s, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) { FWD(IASetVertexBuffers(s, n, v)); }
void STDMETHODCALLTYPE Dxr11CommandList::SOSetTargets(UINT s, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* v) { FWD(SOSetTargets(s, n, v)); }
void STDMETHODCALLTYPE Dxr11CommandList::OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rt, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* ds) { FWD(OMSetRenderTargets(n, rt, single, ds)); }
void STDMETHODCALLTYPE Dxr11CommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE v, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s, UINT n, const D3D12_RECT* r) { FWD(ClearDepthStencilView(v, f, d, s, n, r)); }
void STDMETHODCALLTYPE Dxr11CommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE v, const FLOAT c[4], UINT n, const D3D12_RECT* r) { FWD(ClearRenderTargetView(v, c, n, r)); }
void STDMETHODCALLTYPE Dxr11CommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res, const UINT v[4], UINT n, const D3D12_RECT* r) { FWD(ClearUnorderedAccessViewUint(g, c, res, v, n, r)); }
void STDMETHODCALLTYPE Dxr11CommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* res, const FLOAT v[4], UINT n, const D3D12_RECT* r) { FWD(ClearUnorderedAccessViewFloat(g, c, res, v, n, r)); }
void STDMETHODCALLTYPE Dxr11CommandList::DiscardResource(ID3D12Resource* r, const D3D12_DISCARD_REGION* d) { FWD(DiscardResource(r, d)); }
void STDMETHODCALLTYPE Dxr11CommandList::BeginQuery(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) { FWD(BeginQuery(h, t, i)); }
void STDMETHODCALLTYPE Dxr11CommandList::EndQuery(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) { FWD(EndQuery(h, t, i)); }
void STDMETHODCALLTYPE Dxr11CommandList::ResolveQueryData(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT s, UINT n, ID3D12Resource* d, UINT64 o) { FWD(ResolveQueryData(h, t, s, n, d, o)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetPredication(ID3D12Resource* b, UINT64 o, D3D12_PREDICATION_OP op) { FWD(SetPredication(b, o, op)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetMarker(UINT m, const void* d, UINT s) { FWD(SetMarker(m, d, s)); }
void STDMETHODCALLTYPE Dxr11CommandList::BeginEvent(UINT m, const void* d, UINT s) { FWD(BeginEvent(m, d, s)); }
void STDMETHODCALLTYPE Dxr11CommandList::EndEvent() { FWD(EndEvent()); }

// The seat this whole wrapper exists to create. Still a pure forward; the
// DISPATCH_RAYS substitution and the command list split land here.
void STDMETHODCALLTYPE Dxr11CommandList::ExecuteIndirect(ID3D12CommandSignature* sig, UINT maxCount, ID3D12Resource* args, UINT64 argOffset, ID3D12Resource* countBuf, UINT64 countOffset) {
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0)
        ProxyLog("[dxr11-proxy] ExecuteIndirect intercepted (first real call): "
                 "sig=%p maxCount=%u\n", (void*)sig, maxCount);
    FWD(ExecuteIndirect(sig, maxCount, args, argOffset, countBuf, countOffset));
}

void STDMETHODCALLTYPE Dxr11CommandList::AtomicCopyBufferUINT(ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT n, ID3D12Resource* const* dep, const D3D12_SUBRESOURCE_RANGE_UINT64* ranges) { FWD(AtomicCopyBufferUINT(d, dof, s, sof, n, dep, ranges)); }
void STDMETHODCALLTYPE Dxr11CommandList::AtomicCopyBufferUINT64(ID3D12Resource* d, UINT64 dof, ID3D12Resource* s, UINT64 sof, UINT n, ID3D12Resource* const* dep, const D3D12_SUBRESOURCE_RANGE_UINT64* ranges) { FWD(AtomicCopyBufferUINT64(d, dof, s, sof, n, dep, ranges)); }
void STDMETHODCALLTYPE Dxr11CommandList::OMSetDepthBounds(FLOAT a, FLOAT b) { FWD(OMSetDepthBounds(a, b)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetSamplePositions(UINT a, UINT b, D3D12_SAMPLE_POSITION* p) { FWD(SetSamplePositions(a, b, p)); }
void STDMETHODCALLTYPE Dxr11CommandList::ResolveSubresourceRegion(ID3D12Resource* d, UINT ds, UINT x, UINT y, ID3D12Resource* s, UINT ss, D3D12_RECT* r, DXGI_FORMAT f, D3D12_RESOLVE_MODE m) { FWD(ResolveSubresourceRegion(d, ds, x, y, s, ss, r, f, m)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetViewInstanceMask(UINT m) { FWD(SetViewInstanceMask(m)); }
void STDMETHODCALLTYPE Dxr11CommandList::WriteBufferImmediate(UINT c, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* p, const D3D12_WRITEBUFFERIMMEDIATE_MODE* m) { FWD(WriteBufferImmediate(c, p, m)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetProtectedResourceSession(ID3D12ProtectedResourceSession* s) { FWD(SetProtectedResourceSession(s)); }
void STDMETHODCALLTYPE Dxr11CommandList::BeginRenderPass(UINT n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rt, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS f) { FWD(BeginRenderPass(n, rt, ds, f)); }
void STDMETHODCALLTYPE Dxr11CommandList::EndRenderPass() { FWD(EndRenderPass()); }
void STDMETHODCALLTYPE Dxr11CommandList::InitializeMetaCommand(ID3D12MetaCommand* m, const void* d, SIZE_T s) { FWD(InitializeMetaCommand(m, d, s)); }
void STDMETHODCALLTYPE Dxr11CommandList::ExecuteMetaCommand(ID3D12MetaCommand* m, const void* d, SIZE_T s) { FWD(ExecuteMetaCommand(m, d, s)); }
void STDMETHODCALLTYPE Dxr11CommandList::BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT n, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* p) { FWD(BuildRaytracingAccelerationStructure(d, n, p)); }
void STDMETHODCALLTYPE Dxr11CommandList::EmitRaytracingAccelerationStructurePostbuildInfo(const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d, UINT n, const D3D12_GPU_VIRTUAL_ADDRESS* a) { FWD(EmitRaytracingAccelerationStructurePostbuildInfo(d, n, a)); }
void STDMETHODCALLTYPE Dxr11CommandList::CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS d, D3D12_GPU_VIRTUAL_ADDRESS s, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE m) { FWD(CopyRaytracingAccelerationStructure(d, s, m)); }
void STDMETHODCALLTYPE Dxr11CommandList::SetPipelineState1(ID3D12StateObject* s) { FWD(SetPipelineState1(s)); }
void STDMETHODCALLTYPE Dxr11CommandList::DispatchRays(const D3D12_DISPATCH_RAYS_DESC* d) { FWD(DispatchRays(d)); }

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
void STDMETHODCALLTYPE Dxr11CommandList::DispatchMesh(UINT x, UINT y, UINT z) {
    if (m_real6) m_real6->DispatchMesh(x, y, z);
}
