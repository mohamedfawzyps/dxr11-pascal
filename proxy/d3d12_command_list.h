// Phase 4 (S1) - ID3D12GraphicsCommandList4 wrapper.
//
// The other half of the hybrid. The queue is hooked, the list is wrapped, and
// both choices are forced by measurement rather than taste. See
// docs/phase4-indirect-design.md, and the reasoning summary in queue_hook.h.
//
// Wrapping is safe here precisely because nothing outside D3D12 ever holds a
// command list. The one place a wrapper would leak out is
// ID3D12CommandQueue::ExecuteCommandLists, and that is exactly what the queue
// vtable hook exists to intercept and unwrap.
//
// Right now every method is a straight forward. The point of the wrapper is the
// seat it creates: ExecuteIndirect for the DISPATCH_RAYS substitution, and the
// binding calls, which segmentation has to replay after a split because Reset
// clears all of them.

#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

// --- command list splitting -------------------------------------------------
//
// An indirect ray dispatch cannot be serviced at record time when its argument
// buffer lives on the GPU, because the dimensions do not exist yet. The shim
// therefore records into a SEQUENCE of real command lists, splitting at the
// ExecuteIndirect, and the queue hook plays them back as:
//
//     submit segment N  ->  wait  ->  read the dimensions  ->
//     record and submit a dispatch-only list  ->  submit segment N+1
//
// The dispatch list can only be recorded once the dimensions are known, which is
// why it is built at submit time rather than during recording.
//
// Splitting costs the binding state: Reset clears everything, so whatever the
// application had set has to be replayed at the head of the continuation
// segment, and again in the dispatch list. That is what Dxr11Bindings is for.

// One root parameter's last-set value on the compute pipeline, which is the one
// DispatchRays uses.
struct Dxr11RootParam {
    enum Kind : unsigned char { None, Table, Constants, CBV, SRV, UAV };
    Kind kind = None;
    D3D12_GPU_DESCRIPTOR_HANDLE table{};
    D3D12_GPU_VIRTUAL_ADDRESS   address = 0;
    std::vector<UINT>           constants;
};

// Enough state to make a DispatchRays behave as the application intended. This
// is the COMPUTE pipeline only, which is what DispatchRays uses, and it is all
// the dispatch-only list needs.
struct Dxr11Bindings {
    static const UINT kMaxRootParams = 64;   // a root signature is 64 DWORDs

    ID3D12RootSignature* rootSig = nullptr;
    ID3D12StateObject*   stateObject = nullptr;
    std::vector<ID3D12DescriptorHeap*> heaps;
    Dxr11RootParam roots[kMaxRootParams];

    void Retain();      // AddRef everything held
    void ReleaseAll();  // and let it go
    // Replays in dependency order: heaps, then root signature (which clears the
    // root parameters), then the parameters, then the state object.
    void Replay(ID3D12GraphicsCommandList4* cl) const;
};

// Everything else Reset clears.
//
// The dispatch-only list does not need any of this. The CONTINUATION segment
// does: the application carries on recording into it believing its graphics
// state is still bound, and a fresh command list has none. Tracking only root
// parameters would not be enough, since a draw also needs its pipeline state,
// topology, viewports, targets and buffers.
struct Dxr11GfxState {
    static const UINT kMaxRootParams = 64;
    static const UINT kMaxRTs = 8;
    static const UINT kMaxVBs = 32;

    ID3D12PipelineState* pso = nullptr;
    ID3D12RootSignature* rootSig = nullptr;
    Dxr11RootParam       roots[kMaxRootParams];

    bool hasTopology = false;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    std::vector<D3D12_VIEWPORT> viewports;
    std::vector<D3D12_RECT>     scissors;

    bool  hasBlendFactor = false;  FLOAT blendFactor[4] = { 0, 0, 0, 0 };
    bool  hasStencilRef = false;   UINT  stencilRef = 0;

    bool hasRTs = false;
    UINT numRTs = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtHandles[kMaxRTs]{};
    BOOL rtsSingleHandle = FALSE;
    bool hasDSV = false;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};

    bool hasIB = false;  D3D12_INDEX_BUFFER_VIEW ib{};
    unsigned long vbMask = 0;  D3D12_VERTEX_BUFFER_VIEW vbs[kMaxVBs]{};

    bool  hasDepthBounds = false;  FLOAT depthMin = 0, depthMax = 1;
    bool  hasViewInstanceMask = false;  UINT viewInstanceMask = 0;

    void Retain();
    void ReleaseAll();
    void Replay(ID3D12GraphicsCommandList4* cl) const;

    // NOT tracked, and nothing has needed it yet: stream output targets,
    // predication, sample positions and shading rate. Each would be a few more
    // fields here if an application turns out to set them across a split.
};

// A dispatch that could not be issued at record time. The readback buffer
// receives the argument buffer's contents once the preceding segment has run.
struct Dxr11PendingDispatch {
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    UINT64        readbackOffset = 0;
    Dxr11Bindings bindings;
    // Set for an indirect COMPUTE dispatch of a lowered RayQuery pipeline,
    // where the readback holds three group counts rather than a
    // D3D12_DISPATCH_RAYS_DESC. `rqRef` holds the reference that keeps `rq`
    // alive until the dispatch is issued; `counts` is the capture's own UAV.
    class Dxr11RayQueryPso*                      rq = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  rqRef;
    Microsoft::WRL::ComPtr<ID3D12Resource>       counts;
    // The scene it traces, resolved when it was recorded; `rqExact` false
    // means not resolved, judged over every live structure.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS>       rqScenes;
    bool                                         rqExact = false;
};

// A closed segment, followed by the dispatches that could not be recorded until
// its contents had run. Several dispatches share one segment, and therefore one
// sync, whenever the application issued them with no GPU work in between.
struct Dxr11Segment {
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> list;  // closed
    std::vector<Dxr11PendingDispatch> pendings;
};

// A dispatch-only list and its allocator, reusable once the GPU has passed the
// fence value recorded when it was submitted. Pooling is what lets the submit
// path signal without blocking: the old code waited after every dispatch purely
// to know the list was safe to reuse, which doubled the number of stalls.
struct Dxr11DispatchList {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>     alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> list;
    UINT64 fenceValue = 0;
};

// Private interface used only so the queue hook can tell one of our wrappers
// from a real command list. Not exposed to applications.
// {A6B41C7E-2E5D-4C3B-9F80-1D4E6A0F2B91}
extern const GUID IID_Dxr11CommandList;

class Dxr11RayQueryPso;
namespace gidx { struct Scenes; }

class Dxr11CommandList : public ID3D12GraphicsCommandList10 {
public:
    // Takes ownership of one reference on `real`, and acquires its own on the
    // GraphicsCommandList5/6 interfaces if the runtime offers them.
    explicit Dxr11CommandList(ID3D12GraphicsCommandList4* real);
    ~Dxr11CommandList();

    ID3D12GraphicsCommandList4* Real() const { return m_real; }

    // Returns the real list if `maybeWrapped` is one of ours, else null.
    // Does not change the reference count of the returned pointer.
    static ID3D12CommandList* Unwrap(ID3D12CommandList* maybeWrapped);

    // Returns the wrapper if `maybe` is one of ours, else null. No ref change.
    static Dxr11CommandList* From(ID3D12CommandList* maybe);

    // True when this recording was split and needs the staged submission below.
    bool IsSplit() const { return !m_segments.empty(); }

    // Plays the recording back on `queue`, syncing at each split to read the
    // dispatch dimensions. `submit` is the ORIGINAL ExecuteCommandLists, since
    // ours is hooked. Returns false if it could not be done, in which case the
    // caller should fall back to submitting the list as-is.
    typedef void (STDMETHODCALLTYPE *SubmitFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
    bool SubmitSegmented(ID3D12CommandQueue* queue, SubmitFn submit);

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    // --- ID3D12Object ---
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR Name) override;

    // --- ID3D12DeviceChild ---
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppvDevice) override;

    // --- ID3D12CommandList ---
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override;

    // --- ID3D12GraphicsCommandList ---
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pInitialState) override;
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pPipelineState) override;
    void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override;
    void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override;
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION* pSrc, const D3D12_BOX* pSrcBox) override;
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource* pDstResource, ID3D12Resource* pSrcResource) override;
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource* pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate, const D3D12_TILE_REGION_SIZE* pTileRegionSize, ID3D12Resource* pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource* pDstResource, UINT DstSubresource, ID3D12Resource* pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT* pViewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT BlendFactor[4]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT StencilRef) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pPipelineState) override;
    void STDMETHODCALLTYPE ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) override;
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList* pCommandList) override;
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT NumDescriptorHeaps, ID3D12DescriptorHeap* const* ppDescriptorHeaps) override;
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* pRootSignature) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* pRootSignature) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void* pSrcData, UINT DestOffsetIn32BitValues) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* pView) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW* pViews) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* pViews) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource* pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT* pRects) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource* pResource, const D3D12_DISCARD_REGION* pRegion) override;
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override;
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index) override;
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries, ID3D12Resource* pDestinationBuffer, UINT64 AlignedDestinationBufferOffset) override;
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource* pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) override;
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void* pData, UINT Size) override;
    void STDMETHODCALLTYPE EndEvent() override;
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset) override;

    // --- ID3D12GraphicsCommandList1 ---
    void STDMETHODCALLTYPE AtomicCopyBufferUINT(ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies, ID3D12Resource* const* ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) override;
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(ID3D12Resource* pDstBuffer, UINT64 DstOffset, ID3D12Resource* pSrcBuffer, UINT64 SrcOffset, UINT Dependencies, ID3D12Resource* const* ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64* pDependentSubresourceRanges) override;
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT Min, FLOAT Max) override;
    void STDMETHODCALLTYPE SetSamplePositions(UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION* pSamplePositions) override;
    void STDMETHODCALLTYPE ResolveSubresourceRegion(ID3D12Resource* pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, ID3D12Resource* pSrcResource, UINT SrcSubresource, D3D12_RECT* pSrcRect, DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode) override;
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT Mask) override;

    // --- ID3D12GraphicsCommandList2 ---
    void STDMETHODCALLTYPE WriteBufferImmediate(UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* pParams, const D3D12_WRITEBUFFERIMMEDIATE_MODE* pModes) override;

    // --- ID3D12GraphicsCommandList3 ---
    void STDMETHODCALLTYPE SetProtectedResourceSession(ID3D12ProtectedResourceSession* pProtectedResourceSession) override;

    // --- ID3D12GraphicsCommandList4 ---
    void STDMETHODCALLTYPE BeginRenderPass(UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil, D3D12_RENDER_PASS_FLAGS Flags) override;
    void STDMETHODCALLTYPE EndRenderPass() override;
    void STDMETHODCALLTYPE InitializeMetaCommand(ID3D12MetaCommand* pMetaCommand, const void* pInitializationParametersData, SIZE_T InitializationParametersDataSizeInBytes) override;
    void STDMETHODCALLTYPE ExecuteMetaCommand(ID3D12MetaCommand* pMetaCommand, const void* pExecutionParametersData, SIZE_T ExecutionParametersDataSizeInBytes) override;
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc, UINT NumPostbuildInfoDescs, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs) override;
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pDesc, UINT NumSourceAccelerationStructures, const D3D12_GPU_VIRTUAL_ADDRESS* pSourceAccelerationStructureData) override;
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData, D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode) override;
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject* pStateObject) override;
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC* pDesc) override;

    // --- ID3D12GraphicsCommandList5 ---
    // Covered because the log caught SimpleLighting querying for
    // ID3D12GraphicsCommandList5 and receiving an UNWRAPPED list, which would
    // have bypassed our ExecuteIndirect entirely.
    void STDMETHODCALLTYPE RSSetShadingRate(D3D12_SHADING_RATE baseShadingRate, const D3D12_SHADING_RATE_COMBINER* combiners) override;
    void STDMETHODCALLTYPE RSSetShadingRateImage(ID3D12Resource* shadingRateImage) override;

    // --- ID3D12GraphicsCommandList6 ---
    void STDMETHODCALLTYPE DispatchMesh(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;

    // --- ID3D12GraphicsCommandList7 ---
    void STDMETHODCALLTYPE Barrier(UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP *pBarrierGroups) override;

    // --- ID3D12GraphicsCommandList8 ---
    void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef) override;

    // --- ID3D12GraphicsCommandList9 ---
    void STDMETHODCALLTYPE RSSetDepthBias(FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias) override;
    void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue) override;

    // --- ID3D12GraphicsCommandList10 ---
    void STDMETHODCALLTYPE SetProgram(const D3D12_SET_PROGRAM_DESC *pDesc) override;
    void STDMETHODCALLTYPE DispatchGraph(const D3D12_DISPATCH_GRAPH_DESC *pDesc) override;

private:
    // Ends the current segment at an indirect dispatch: copies the arguments to
    // a readback buffer, closes the segment, opens a fresh one and replays the
    // bindings into it. Returns false if it could not, and the caller then
    // refuses the dispatch rather than issuing a wrong one.
    // Records the readback copy for an indirect dispatch and queues it, WITHOUT
    // closing the segment. The close is deferred so that a run of dispatches
    // with no work between them lands in one segment behind one sync.
    bool QueueSplit(ID3D12Resource* args, UINT64 argOffset);
    // The same, for ExecuteIndirect with a DISPATCH signature while a lowered
    // RayQuery pipeline is bound. See proxy/group_count.h.
    bool QueueIndirectCompute(ID3D12CommandSignature* sig, ID3D12Resource* args,
                              UINT64 argOffset);
    // Gets the instance descriptions of a top-level build to the CPU, so the
    // shader table can know the contributions and the geometry types they
    // reach. Reads them outright when they are in CPU-visible memory;
    // otherwise records a copy that the queue hook picks up later.
    void CaptureInstances(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc);
    // Closes the segment if any dispatches are queued, and opens a continuation
    // with the bindings replayed. Called before recording anything that must be
    // ordered after those dispatches.
    void FlushQueuedSplit();
    // True when a dispatch is queued and the caller is about to record work.
    void WorkBarrier() { if (!m_openPendings.empty()) FlushQueuedSplit(); }
    // After a capture borrowed the compute bindings: put them back.
    void RestoreComputeAfterCapture();
    // The scenes `sc` names, resolved through the bound compute root
    // signature (gidx::ResolveScenes). False with *why when not resolved.
    bool ResolveBoundScenes(const gidx::Scenes& sc, std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* out,
                            std::string* why);
    // The same for a lowered RayQuery pipeline, counted and logged.
    bool RayQueryScenes(Dxr11RayQueryPso* rq, std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* out);
    ID3D12Device5* RealDevice();

    ID3D12GraphicsCommandList4* m_real;
    // Null when the runtime does not offer them; QueryInterface then refuses the
    // matching IID rather than handing back a vtable it cannot honour.
    ID3D12GraphicsCommandList5* m_real5;
    // The lowered RayQuery pipeline currently bound, if any. Not owned:
    // the application holds the reference, and it must outlive its own
    // Dispatch calls for any pipeline state, ours included.
    Dxr11RayQueryPso* m_rqPso = nullptr;
    // Whether SetPipelineState1 came after the last SetPipelineState, so a
    // capture restores the one the application bound last.
    bool m_lastBoundStateObject = false;

public:
    // Set the stand-in on a freshly wrapped list, for the case where the
    // application named it as CreateCommandList's initial state and never
    // calls SetPipelineState afterwards.
    static void AdoptRayQueryPso(void* wrappedList, Dxr11RayQueryPso* rq,
                                 ID3D12PipelineState* initial);
    // A list created OPEN records into the allocator it was created with, and
    // a split needs that allocator. Only Reset used to record it, so a list
    // split before its first Reset could not split: 0.39.0 skipped Unreal's
    // indirect dispatches with "no device or allocator".
    static void AdoptAllocator(void* wrappedList, ID3D12CommandAllocator* a);

private:

    ID3D12GraphicsCommandList6* m_real6;
    ID3D12GraphicsCommandList7* m_real7;
    ID3D12GraphicsCommandList8* m_real8;
    ID3D12GraphicsCommandList9* m_real9;
    ID3D12GraphicsCommandList10* m_real10;
    LONG                        m_refs;

    // Split machinery. Empty and untouched for a recording with no indirect ray
    // dispatch, which is every recording in every app tested so far.
    std::vector<Dxr11Segment>                          m_segments;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>     m_allocator;   // the app's, from Reset
    std::vector<Dxr11PendingDispatch>                  m_openPendings; // queued, not yet closed
    std::vector<Dxr11DispatchList>                     m_dispatchPool;
    Microsoft::WRL::ComPtr<ID3D12Fence>                m_fence;
    UINT64                                             m_fenceValue = 0;
    Microsoft::WRL::ComPtr<ID3D12Device5>              m_device;
    Dxr11Bindings                                      m_bindings;   // compute
    Dxr11GfxState                                      m_gfx;        // everything else
};
