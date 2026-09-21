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

    // True when the lowered shader commits PROCEDURAL hits, so its hit group
    // carries an intersection shader instead of an any-hit one. That decides
    // whether the scene's geometry can be served by it, which the caller asks
    // the acceleration structure tracker about.
    bool CommitsProcedural() const { return m_isProcedural; }

    // Issue the work the application asked for as Dispatch. The ray grid is
    // the thread group count times the shader's numthreads, because the
    // lowered raygen reads DispatchRaysIndex where the original read
    // SV_DispatchThreadID.
    //
    // `hitRecords` is how many hit group records the scene needs, which is one
    // more than the largest InstanceContributionToHitGroupIndex any instance
    // carries. The caller supplies it because it comes from the acceleration
    // structures, which this class knows nothing about. The table grows to fit
    // if it has to; growing is what makes a nonzero contribution work at all.
    void DispatchAsRays(ID3D12GraphicsCommandList4* cl, UINT gx, UINT gy, UINT gz,
                        UINT hitRecords);

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

    // (Re)build the shader table with `hitRecords` hit group records. Every
    // record holds the SAME identifier, because the shim has one hit group and
    // wants it to run whichever slot the application's layout routes a hit to.
    // Only the number of slots has to match the application; what is in them
    // does not.
    bool BuildTable(UINT hitRecords, std::string* why);

    ID3D12Device5* m_dev = nullptr;
    ID3D12StateObject* m_so = nullptr;
    ID3D12RootSignature* m_rootSig = nullptr;   // the app's, kept alive
    ID3D12Resource* m_sbt = nullptr;            // raygen, miss, hit, one buffer
    // Tables replaced by a growth. A dispatch recorded against the old one may
    // still be in flight, and nothing here knows when it lands, so they are
    // held until the pipeline itself dies. Growth is monotonic and rare, so
    // this stays a handful of small buffers.
    std::vector<ID3D12Resource*> m_retired;
    UINT m_hitRecords = 0;
    bool m_isProcedural = false;
    // The three identifiers, kept so the table can be rebuilt without going
    // back to the state object.
    uint8_t m_idRay[32]{}, m_idMiss[32]{}, m_idHit[32]{};
    D3D12_DISPATCH_RAYS_DESC m_desc{};
    UINT m_threads[3] = { 1, 1, 1 };
    LONG m_refs = 1;
};
