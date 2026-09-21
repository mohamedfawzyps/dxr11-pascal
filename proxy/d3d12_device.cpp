// Phase 3b - ID3D12Device5 wrapper implementation.
//
// Every method here is a straight forward to the real device. The only methods
// with any logic of their own are QueryInterface, AddRef and Release.
//
// Keep it that way until Phase 4. When behaviour does get added, it belongs in
// the handful of methods named in d3d12_device.h, and each one should say why
// it diverges.

#include "d3d12_device.h"
#include "proxy_log.h"
#include "state_object_cache.h"
#include "queue_hook.h"

#include <windows.h>
#include <new>
#include <string>

// FWD(method, args...) - forward to the real device, returning its result.
#define FWD(call) return m_real->call

Dxr11Device::Dxr11Device(ID3D12Device5* real)
    : m_real(real), m_tier11(false), m_real6(nullptr), m_real7(nullptr), m_refs(1) {
    if (m_real) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        if (SUCCEEDED(m_real->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))))
            m_tier11 = (o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
    }
    // Optional: a device that stops at Device5 is still wrappable, we just
    // decline the higher IIDs in QueryInterface.
    if (m_real) {
        m_real->QueryInterface(__uuidof(ID3D12Device6), (void**)&m_real6);
        m_real->QueryInterface(__uuidof(ID3D12Device7), (void**)&m_real7);
    }
    ProxyLog("[dxr11-proxy] device wrapper created (real=%p, Device6=%s, Device7=%s, tier=%s)\n",
             (void*)m_real, m_real6 ? "yes" : "no", m_real7 ? "yes" : "no",
             m_tier11 ? "1.1" : "1.0");
}

Dxr11Device::~Dxr11Device() {
    if (m_real7) m_real7->Release();
    if (m_real6) m_real6->Release();
    if (m_real)  m_real->Release();
}

// --- IUnknown ---------------------------------------------------------------
//
// We answer for the device interfaces we actually derive from, and hand
// everything else to the real device. Handing it over is the right call for the
// debug interfaces (ID3D12DebugDevice, ID3D12InfoQueue) where the app wants the
// real object. It is a hole for device interfaces ABOVE Device5, most
// importantly ID3D12Device7, whose AddToStateObject is a Phase 4 target: the
// app would get an unwrapped device and bypass us entirely. So log those
// loudly rather than let them pass silently.

HRESULT STDMETHODCALLTYPE Dxr11Device::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)      || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12Device)  || riid == __uuidof(ID3D12Device1) ||
        riid == __uuidof(ID3D12Device2) || riid == __uuidof(ID3D12Device3) ||
        riid == __uuidof(ID3D12Device4) || riid == __uuidof(ID3D12Device5) ||
        (riid == __uuidof(ID3D12Device6) && m_real6) ||
        (riid == __uuidof(ID3D12Device7) && m_real7)) {
        AddRef();
        *ppvObject = static_cast<ID3D12Device7*>(this);
        return S_OK;
    }

    // Asked for Device6/7 but the real device does not have it. Say so rather
    // than handing back a pointer whose vtable the device cannot honour.
    if (riid == __uuidof(ID3D12Device6) || riid == __uuidof(ID3D12Device7))
        return E_NOINTERFACE;

    HRESULT hr = m_real->QueryInterface(riid, ppvObject);
    if (SUCCEEDED(hr)) {
        ProxyLog("[dxr11-proxy] device QI PASSED THROUGH UNWRAPPED: %s\n",
                 ProxyIidName(riid));
    }
    return hr;
}

ULONG STDMETHODCALLTYPE Dxr11Device::AddRef() {
    return (ULONG)InterlockedIncrement(&m_refs);
}

ULONG STDMETHODCALLTYPE Dxr11Device::Release() {
    LONG n = InterlockedDecrement(&m_refs);
    if (n == 0) {
        ProxyLog("[dxr11-proxy] device wrapper destroyed (real=%p)\n", (void*)m_real);
        delete this;
    }
    return (ULONG)n;
}

// --- ID3D12Object -----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) { FWD(GetPrivateData(guid, pDataSize, pData)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) { FWD(SetPrivateData(guid, DataSize, pData)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) { FWD(SetPrivateDataInterface(guid, pData)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::SetName(LPCWSTR Name) { FWD(SetName(Name)); }

// --- ID3D12Device -----------------------------------------------------------

UINT STDMETHODCALLTYPE Dxr11Device::GetNodeCount() { FWD(GetNodeCount()); }
// NOT wrapped, and this is a measured constraint rather than an oversight.
// DXGI consumes the queue: the app hands it to CreateSwapChainForHwnd, and with
// a wrapper in the way the first Present access-violates. See
// docs/phase4-indirect-design.md and tier11probe.exe -dxgiqueue.
//
// Instead the queue's vtable is hooked, once, for ExecuteCommandLists. That is
// safe here and not for command lists, because every queue of every type shares
// one image-address vtable that does not change under use, verified with
// tier11probe.exe -queuevtable.
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppCommandQueue) {
    HRESULT hr = m_real->CreateCommandQueue(pDesc, riid, ppCommandQueue);
    if (m_tier11 || FAILED(hr) || !ppCommandQueue || !*ppCommandQueue) return hr;
    ID3D12CommandQueue* q = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(*ppCommandQueue)->QueryInterface(
            __uuidof(ID3D12CommandQueue), (void**)&q)) && q) {
        Dxr11InstallQueueHook(q);
        q->Release();
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** ppCommandAllocator) { FWD(CreateCommandAllocator(type, riid, ppCommandAllocator)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) { FWD(CreateGraphicsPipelineState(pDesc, riid, ppPipelineState)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) { FWD(CreateComputePipelineState(pDesc, riid, ppPipelineState)); }
// Neither wrapped nor hooked, and both were tried and measured.
//
// Wrapping the list forces wrapping the queue, and a wrapped queue makes DXGI
// access-violate on the first Present.
//
// Hooking the list vtable does not work either: a command list swaps to a
// PER-OBJECT, heap-allocated vtable, so the shared image vtable present when
// CreateCommandList returns is not the one in use when the app records. A hook
// installed here self-tests cleanly and then never sees a single real call.
// Measured with `tier11probe.exe -hooktest`; see docs/phase4-indirect-design.md.
//
// The queue vtable, by contrast, is an image address and is shared, which is
// what the eventual design leans on.
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) { FWD(CreateCommandList(nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CheckFeatureSupport(D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) { FWD(CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) { FWD(CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap)); }
UINT STDMETHODCALLTYPE Dxr11Device::GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) { FWD(GetDescriptorHandleIncrementSize(DescriptorHeapType)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateRootSignature(UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes, REFIID riid, void** ppvRootSignature) { FWD(CreateRootSignature(nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature)); }
void STDMETHODCALLTYPE Dxr11Device::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) { FWD(CreateConstantBufferView(pDesc, DestDescriptor)); }
void STDMETHODCALLTYPE Dxr11Device::CreateShaderResourceView(ID3D12Resource* pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) { FWD(CreateShaderResourceView(pResource, pDesc, DestDescriptor)); }
void STDMETHODCALLTYPE Dxr11Device::CreateUnorderedAccessView(ID3D12Resource* pResource, ID3D12Resource* pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) { FWD(CreateUnorderedAccessView(pResource, pCounterResource, pDesc, DestDescriptor)); }
void STDMETHODCALLTYPE Dxr11Device::CreateRenderTargetView(ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) { FWD(CreateRenderTargetView(pResource, pDesc, DestDescriptor)); }
void STDMETHODCALLTYPE Dxr11Device::CreateDepthStencilView(ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) { FWD(CreateDepthStencilView(pResource, pDesc, DestDescriptor)); }
void STDMETHODCALLTYPE Dxr11Device::CreateSampler(const D3D12_SAMPLER_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) { FWD(CreateSampler(pDesc, DestDescriptor)); }
void STDMETHODCALLTYPE Dxr11Device::CopyDescriptors(UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pDestDescriptorRangeStarts, const UINT* pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pSrcDescriptorRangeStarts, const UINT* pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) { FWD(CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes, NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType)); }
void STDMETHODCALLTYPE Dxr11Device::CopyDescriptorsSimple(UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart, D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType) { FWD(CopyDescriptorsSimple(NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart, DescriptorHeapsType)); }
D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE Dxr11Device::GetResourceAllocationInfo(UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs) { FWD(GetResourceAllocationInfo(visibleMask, numResourceDescs, pResourceDescs)); }
D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE Dxr11Device::GetCustomHeapProperties(UINT nodeMask, D3D12_HEAP_TYPE heapType) { FWD(GetCustomHeapProperties(nodeMask, heapType)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) { FWD(CreateCommittedResource(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidResource, ppvResource)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateHeap(const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) { FWD(CreateHeap(pDesc, riid, ppvHeap)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreatePlacedResource(ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) { FWD(CreatePlacedResource(pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateReservedResource(const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) { FWD(CreateReservedResource(pDesc, InitialState, pOptimizedClearValue, riid, ppvResource)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateSharedHandle(ID3D12DeviceChild* pObject, const SECURITY_ATTRIBUTES* pAttributes, DWORD Access, LPCWSTR Name, HANDLE* pHandle) { FWD(CreateSharedHandle(pObject, pAttributes, Access, Name, pHandle)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::OpenSharedHandle(HANDLE NTHandle, REFIID riid, void** ppvObj) { FWD(OpenSharedHandle(NTHandle, riid, ppvObj)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::OpenSharedHandleByName(LPCWSTR Name, DWORD Access, HANDLE* pNTHandle) { FWD(OpenSharedHandleByName(Name, Access, pNTHandle)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::MakeResident(UINT NumObjects, ID3D12Pageable* const* ppObjects) { FWD(MakeResident(NumObjects, ppObjects)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::Evict(UINT NumObjects, ID3D12Pageable* const* ppObjects) { FWD(Evict(NumObjects, ppObjects)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateFence(UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void** ppFence) { FWD(CreateFence(InitialValue, Flags, riid, ppFence)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::GetDeviceRemovedReason() { FWD(GetDeviceRemovedReason()); }
void STDMETHODCALLTYPE Dxr11Device::GetCopyableFootprints(const D3D12_RESOURCE_DESC* pResourceDesc, UINT FirstSubresource, UINT NumSubresources, UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* pLayouts, UINT* pNumRows, UINT64* pRowSizeInBytes, UINT64* pTotalBytes) { FWD(GetCopyableFootprints(pResourceDesc, FirstSubresource, NumSubresources, BaseOffset, pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) { FWD(CreateQueryHeap(pDesc, riid, ppvHeap)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::SetStablePowerState(BOOL Enable) { FWD(SetStablePowerState(Enable)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature, REFIID riid, void** ppvCommandSignature) { FWD(CreateCommandSignature(pDesc, pRootSignature, riid, ppvCommandSignature)); }
void STDMETHODCALLTYPE Dxr11Device::GetResourceTiling(ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource, D3D12_PACKED_MIP_INFO* pPackedMipDesc, D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips, UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet, D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) { FWD(GetResourceTiling(pTiledResource, pNumTilesForEntireResource, pPackedMipDesc, pStandardTileShapeForNonPackedMips, pNumSubresourceTilings, FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips)); }
LUID STDMETHODCALLTYPE Dxr11Device::GetAdapterLuid() { FWD(GetAdapterLuid()); }

// --- ID3D12Device1 ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::CreatePipelineLibrary(const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid, void** ppPipelineLibrary) { FWD(CreatePipelineLibrary(pLibraryBlob, BlobLength, riid, ppPipelineLibrary)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::SetEventOnMultipleFenceCompletion(ID3D12Fence* const* ppFences, const UINT64* pFenceValues, UINT NumFences, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent) { FWD(SetEventOnMultipleFenceCompletion(ppFences, pFenceValues, NumFences, Flags, hEvent)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::SetResidencyPriority(UINT NumObjects, ID3D12Pageable* const* ppObjects, const D3D12_RESIDENCY_PRIORITY* pPriorities) { FWD(SetResidencyPriority(NumObjects, ppObjects, pPriorities)); }

// --- ID3D12Device2 ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** ppPipelineState) { FWD(CreatePipelineState(pDesc, riid, ppPipelineState)); }

// --- ID3D12Device3 ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::OpenExistingHeapFromAddress(const void* pAddress, REFIID riid, void** ppvHeap) { FWD(OpenExistingHeapFromAddress(pAddress, riid, ppvHeap)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::OpenExistingHeapFromFileMapping(HANDLE hFileMapping, REFIID riid, void** ppvHeap) { FWD(OpenExistingHeapFromFileMapping(hFileMapping, riid, ppvHeap)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::EnqueueMakeResident(D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable* const* ppObjects, ID3D12Fence* pFenceToSignal, UINT64 FenceValueToSignal) { FWD(EnqueueMakeResident(Flags, NumObjects, ppObjects, pFenceToSignal, FenceValueToSignal)); }

// --- ID3D12Device4 ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommandList1(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** ppCommandList) { FWD(CreateCommandList1(nodeMask, type, flags, riid, ppCommandList)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc, REFIID riid, void** ppSession) { FWD(CreateProtectedResourceSession(pDesc, riid, ppSession)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommittedResource1(const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession, REFIID riidResource, void** ppvResource) { FWD(CreateCommittedResource1(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riidResource, ppvResource)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateHeap1(const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession, REFIID riid, void** ppvHeap) { FWD(CreateHeap1(pDesc, pProtectedSession, riid, ppvHeap)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateReservedResource1(const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession, REFIID riid, void** ppvResource) { FWD(CreateReservedResource1(pDesc, InitialState, pOptimizedClearValue, pProtectedSession, riid, ppvResource)); }
D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE Dxr11Device::GetResourceAllocationInfo1(UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC* pResourceDescs, D3D12_RESOURCE_ALLOCATION_INFO1* pResourceAllocationInfo1) { FWD(GetResourceAllocationInfo1(visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1)); }

// --- ID3D12Device5 ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateLifetimeTracker(ID3D12LifetimeOwner* pOwner, REFIID riid, void** ppvTracker) { FWD(CreateLifetimeTracker(pOwner, riid, ppvTracker)); }
void STDMETHODCALLTYPE Dxr11Device::RemoveDevice() { m_real->RemoveDevice(); }
HRESULT STDMETHODCALLTYPE Dxr11Device::EnumerateMetaCommands(UINT* pNumMetaCommands, D3D12_META_COMMAND_DESC* pDescs) { FWD(EnumerateMetaCommands(pNumMetaCommands, pDescs)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::EnumerateMetaCommandParameters(REFGUID CommandId, D3D12_META_COMMAND_PARAMETER_STAGE Stage, UINT* pTotalStructureSizeInBytes, UINT* pParameterCount, D3D12_META_COMMAND_PARAMETER_DESC* pParameterDescs) { FWD(EnumerateMetaCommandParameters(CommandId, Stage, pTotalStructureSizeInBytes, pParameterCount, pParameterDescs)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateMetaCommand(REFGUID CommandId, UINT NodeMask, const void* pCreationParametersData, SIZE_T CreationParametersDataSizeInBytes, REFIID riid, void** ppMetaCommand) { FWD(CreateMetaCommand(CommandId, NodeMask, pCreationParametersData, CreationParametersDataSizeInBytes, riid, ppMetaCommand)); }
// CreateStateObject is the first half of the AddToStateObject emulation.
//
// On Tier 1.0 the driver refuses ALLOW_STATE_OBJECT_ADDITIONS outright
// ("Invalid D3D12_STATE_OBJECT_FLAGS: 0x4"), so the app cannot even build the
// object it intends to grow. We strip the flag, forward, and keep a deep copy of
// what was asked for so AddToStateObject can rebuild from it later.
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateStateObject(const D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid, void** ppStateObject) {
    if (m_tier11 || !pDesc) FWD(CreateStateObject(pDesc, riid, ppStateObject));

    // Only objects the app intends to grow need any of this.
    bool wantsAdditions = false;
    for (UINT i = 0; i < pDesc->NumSubobjects && !wantsAdditions; ++i) {
        const auto& s = pDesc->pSubobjects[i];
        if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG && s.pDesc) {
            const auto* c = static_cast<const D3D12_STATE_OBJECT_CONFIG*>(s.pDesc);
            wantsAdditions = (c->Flags & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS) != 0;
        }
    }
    if (!wantsAdditions) FWD(CreateStateObject(pDesc, riid, ppStateObject));

    auto* store = new (std::nothrow) StateObjectStore();
    if (!store) return E_OUTOFMEMORY;
    std::string why;
    if (!store->Append(*pDesc, false, why)) {
        // Refuse rather than build something subtly different from the request.
        ProxyLog("[dxr11-proxy] CreateStateObject: cannot cache subobjects (%s); "
                 "additions will not work for this object\n", why.c_str());
        delete store;
        FWD(CreateStateObject(pDesc, riid, ppStateObject));
    }
    store->StripAdditionsFlag();

    D3D12_STATE_OBJECT_DESC stripped = store->Desc(pDesc->Type);
    HRESULT hr = m_real->CreateStateObject(&stripped, riid, ppStateObject);
    ProxyLog("[dxr11-proxy] CreateStateObject: stripped ALLOW_STATE_OBJECT_ADDITIONS, "
             "%u subobjects cached, hr=0x%08lx\n",
             (unsigned)store->Count(), (unsigned long)hr);
    if (FAILED(hr) || !ppStateObject || !*ppStateObject) { delete store; return hr; }

    // The cache entry now lives and dies with the state object itself.
    ID3D12StateObject* so = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(*ppStateObject)->QueryInterface(
            __uuidof(ID3D12StateObject), (void**)&so)) && so) {
        StateObjectCacheAttach(so, store);   // takes ownership of `store`
        so->Release();
    } else {
        delete store;
    }
    return hr;
}
void STDMETHODCALLTYPE Dxr11Device::GetRaytracingAccelerationStructurePrebuildInfo(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo) { m_real->GetRaytracingAccelerationStructurePrebuildInfo(pDesc, pInfo); }
D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE Dxr11Device::CheckDriverMatchingIdentifier(D3D12_SERIALIZED_DATA_TYPE SerializedDataType, const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER* pIdentifierToCheck) { FWD(CheckDriverMatchingIdentifier(SerializedDataType, pIdentifierToCheck)); }

// --- ID3D12Device6 ----------------------------------------------------------
//
// Reachable only when QueryInterface handed out a Device6, which it does only
// when m_real6 exists. The null check is belt and braces.

HRESULT STDMETHODCALLTYPE Dxr11Device::SetBackgroundProcessingMode(D3D12_BACKGROUND_PROCESSING_MODE Mode, D3D12_MEASUREMENTS_ACTION MeasurementsAction, HANDLE hEventToSignalUponCompletion, BOOL* pbFurtherMeasurementsDesired) {
    if (!m_real6) return E_NOINTERFACE;
    return m_real6->SetBackgroundProcessingMode(Mode, MeasurementsAction, hEventToSignalUponCompletion, pbFurtherMeasurementsDesired);
}

// --- ID3D12Device7 ----------------------------------------------------------

// The second half of the emulation: rebuild the whole state object from the
// cached subobjects plus the addition.
//
// Rebuilding, rather than genuinely growing, is the point. The driver has no
// incremental path on Tier 1.0, but it will happily build a complete state
// object that happens to contain everything the app has asked for so far. The
// cost is compile time on every addition, which this project explicitly accepts.
HRESULT STDMETHODCALLTYPE Dxr11Device::AddToStateObject(const D3D12_STATE_OBJECT_DESC* pAddition, ID3D12StateObject* pStateObjectToGrowFrom, REFIID riid, void** ppNewStateObject) {
    if (m_tier11 && m_real7) {
        HRESULT hr = m_real7->AddToStateObject(pAddition, pStateObjectToGrowFrom, riid, ppNewStateObject);
        ProxyLog("[dxr11-proxy] AddToStateObject forwarded (tier 1.1) hr=0x%08lx\n",
                 (unsigned long)hr);
        return hr;
    }
    if (!pAddition || !pStateObjectToGrowFrom || !ppNewStateObject) return E_INVALIDARG;

    StateObjectStore* base = StateObjectCacheGet(pStateObjectToGrowFrom);
    if (!base) {
        ProxyLog("[dxr11-proxy] AddToStateObject: no cached subobjects for %p. The "
                 "object was not created through this shim, or its creation did "
                 "not request ALLOW_STATE_OBJECT_ADDITIONS.\n",
                 (void*)pStateObjectToGrowFrom);
        return E_INVALIDARG;
    }

    // Merge into a fresh store so the object being grown keeps its own copy and
    // can still be grown again independently.
    auto* merged = new (std::nothrow) StateObjectStore();
    if (!merged) return E_OUTOFMEMORY;
    std::string why;
    D3D12_STATE_OBJECT_DESC baseDesc = base->Desc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
    if (!merged->Append(baseDesc, false, why) ||
        !merged->Append(*pAddition, /*dropDuplicateSingletons=*/true, why)) {
        ProxyLog("[dxr11-proxy] AddToStateObject: merge failed (%s)\n", why.c_str());
        delete merged;
        return E_INVALIDARG;
    }
    merged->StripAdditionsFlag();

    D3D12_STATE_OBJECT_DESC full = merged->Desc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
    HRESULT hr = m_real->CreateStateObject(&full, riid, ppNewStateObject);
    ProxyLog("[dxr11-proxy] AddToStateObject EMULATED: base %u + addition %u -> "
             "%u subobjects, rebuilt hr=0x%08lx\n",
             (unsigned)baseDesc.NumSubobjects, (unsigned)pAddition->NumSubobjects,
             (unsigned)full.NumSubobjects, (unsigned long)hr);
    if (FAILED(hr) || !*ppNewStateObject) { delete merged; return hr; }

    // The grown object is itself growable, so it needs its own cache entry.
    ID3D12StateObject* so = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(*ppNewStateObject)->QueryInterface(
            __uuidof(ID3D12StateObject), (void**)&so)) && so) {
        StateObjectCacheAttach(so, merged);
        so->Release();
    } else {
        delete merged;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateProtectedResourceSession1(const D3D12_PROTECTED_RESOURCE_SESSION_DESC1* pDesc, REFIID riid, void** ppSession) {
    if (!m_real7) return E_NOINTERFACE;
    return m_real7->CreateProtectedResourceSession1(pDesc, riid, ppSession);
}

// --- creation ---------------------------------------------------------------

HRESULT Dxr11WrapDevice(IUnknown* realDevice, REFIID riid, void** ppDevice) {
    ID3D12Device5* dev5 = nullptr;
    HRESULT hr = realDevice->QueryInterface(__uuidof(ID3D12Device5), (void**)&dev5);
    if (FAILED(hr) || !dev5) {
        // Pre-DXR runtime, or a device that does not reach Device5. Nothing to
        // wrap; the caller keeps the real device.
        ProxyLog("[dxr11-proxy] no ID3D12Device5 on this device (hr=0x%08lx), not wrapping\n",
                 (unsigned long)hr);
        return E_NOINTERFACE;
    }

    Dxr11Device* wrapper = new (std::nothrow) Dxr11Device(dev5);  // consumes the dev5 ref
    if (!wrapper) { dev5->Release(); return E_OUTOFMEMORY; }

    // Hand back whatever interface the caller originally asked for, through our
    // own QueryInterface so the cast is done once, in one place.
    hr = wrapper->QueryInterface(riid, ppDevice);
    wrapper->Release();   // drop our construction ref; QI took its own
    return hr;
}
