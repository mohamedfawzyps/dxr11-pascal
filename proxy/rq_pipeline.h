// The dispatch path: making a lowered RayQuery compute shader actually run.
//
// Rewriting the DXIL is half the job. The application created a COMPUTE
// pipeline and will call Dispatch; the lowered shader is a raytracing LIBRARY
// and needs DispatchRays against a state object and a shader table. Nothing
// the application does changes, so the shim has to own that machinery.
//
// The trick that makes this tractable: **DXR's global root signature IS the
// compute root signature.** SetComputeRootSignature and SetComputeRoot*View
// are exactly what DispatchRays consumes, so the application's bindings carry
// across untouched. Only the pipeline object and the dispatch call are
// substituted.
//
// Dxr11RayQueryPso is that substitution: an ID3D12PipelineState the
// application holds and never inspects, standing in for a state object and
// shader table it knows nothing about. The same shape as Dxr11CommandSignature
// in the indirect DispatchRays work, and for the same reason.
#pragma once

#include <d3d12.h>

#include <string>
#include <vector>

// Private IID, so a pipeline state can be recognised as ours through
// QueryInterface without ever being confused for a real one.
// {7E3B1C42-9A54-4D18-8F60-2C71B0A4E9D3}
extern const GUID IID_Dxr11RayQueryPso;

class Dxr11RayQueryPso : public ID3D12PipelineState {
public:
    // Build everything a lowered RayQuery compute shader needs to run, or
    // explain why not. `why` is filled on failure and the caller forwards the
    // original creation unchanged, so a refusal costs the application nothing
    // beyond the log line.
    static Dxr11RayQueryPso* TryCreate(ID3D12Device5* dev,
                                       const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                       std::string* why);

    // Recognise our own object. Returns null for anything else, including a
    // real pipeline state.
    static Dxr11RayQueryPso* From(ID3D12PipelineState* p);

    // Issue the work the application asked for as Dispatch. The ray grid is
    // the thread group count times the shader's numthreads, because the
    // lowered raygen reads DispatchRaysIndex where the original read
    // SV_DispatchThreadID.
    void DispatchAsRays(ID3D12GraphicsCommandList4* cl, UINT gx, UINT gy, UINT gz);

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // --- ID3D12Object ---
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override;

    // --- ID3D12DeviceChild ---
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** pp) override;

    // --- ID3D12PipelineState ---
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** pp) override;

private:
    Dxr11RayQueryPso() = default;
    ~Dxr11RayQueryPso();

    ID3D12Device5* m_dev = nullptr;
    ID3D12StateObject* m_so = nullptr;
    ID3D12RootSignature* m_rootSig = nullptr;   // the app's, kept alive
    ID3D12Resource* m_sbt = nullptr;            // raygen, miss, hit, one buffer
    D3D12_DISPATCH_RAYS_DESC m_desc{};
    UINT m_threads[3] = { 1, 1, 1 };
    LONG m_refs = 1;
};
