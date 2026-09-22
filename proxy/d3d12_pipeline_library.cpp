#include "d3d12_pipeline_library.h"

#include "proxy_log.h"
#include "rq_pipeline.h"

namespace {

// ID3D12PipelineLibrary1 rather than the base, because an application that
// wants the newer one gets it by QueryInterface on what CreatePipelineLibrary
// returned, and a wrapper that only implements the base would hand the real
// object straight back and lose the interception.
class Dxr11PipelineLibrary : public ID3D12PipelineLibrary1 {
public:
    explicit Dxr11PipelineLibrary(ID3D12PipelineLibrary1* real) : m_real(real) {}

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12PipelineLibrary) ||
            riid == __uuidof(ID3D12PipelineLibrary1)) {
            AddRef();
            *ppv = this;
            return S_OK;
        }
        // Anything else is genuinely not us. Hand over the real object rather
        // than refusing, and say so, the same rule the device wrapper follows.
        return m_real->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --m_refs;
        if (n == 0) { m_real->Release(); delete this; }
        return n;
    }

    // --- ID3D12Object ---
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override {
        return m_real->GetPrivateData(g, s, d);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override {
        return m_real->SetPrivateData(g, s, d);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override {
        return m_real->SetPrivateDataInterface(g, u);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override { return m_real->SetName(n); }

    // --- ID3D12DeviceChild ---
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppv) override {
        return m_real->GetDevice(riid, ppv);
    }

    // --- ID3D12PipelineLibrary ---
    //
    // The whole reason this class exists.
    HRESULT STDMETHODCALLTYPE StorePipeline(LPCWSTR name,
                                            ID3D12PipelineState* pPipeline) override {
        if (Dxr11RayQueryPso::From(pPipeline)) {
            static LONG said = 0;
            if (InterlockedCompareExchange(&said, 1, 0) == 0)
                ProxyLog("[dxr-tier-11-proxy-log] StorePipeline on a lowered RayQuery "
                         "pipeline declined. It is a stand-in, not a real pipeline "
                         "state, and serializing one means the runtime reads fields "
                         "that are not there. The application is told it succeeded; "
                         "the next run simply misses the cache and creates it "
                         "again, which this shim handles.\n");
            return S_OK;
        }
        return m_real->StorePipeline(name, pPipeline);
    }
    HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(
            LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d,
            REFIID riid, void** ppv) override {
        return m_real->LoadGraphicsPipeline(name, d, riid, ppv);
    }
    HRESULT STDMETHODCALLTYPE LoadComputePipeline(
            LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d,
            REFIID riid, void** ppv) override {
        return m_real->LoadComputePipeline(name, d, riid, ppv);
    }
    SIZE_T STDMETHODCALLTYPE GetSerializedSize() override {
        return m_real->GetSerializedSize();
    }
    HRESULT STDMETHODCALLTYPE Serialize(void* p, SIZE_T n) override {
        return m_real->Serialize(p, n);
    }

    // --- ID3D12PipelineLibrary1 ---
    HRESULT STDMETHODCALLTYPE LoadPipeline(
            LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* d,
            REFIID riid, void** ppv) override {
        return m_real->LoadPipeline(name, d, riid, ppv);
    }

private:
    ~Dxr11PipelineLibrary() = default;
    ID3D12PipelineLibrary1* m_real = nullptr;
    ULONG m_refs = 1;
};

}  // namespace

void Dxr11WrapPipelineLibrary(REFIID riid, void** ppv) {
    if (!ppv || !*ppv) return;
    if (riid != __uuidof(ID3D12PipelineLibrary) &&
        riid != __uuidof(ID3D12PipelineLibrary1))
        return;

    // The wrapper implements the 1 interface, so it needs the 1 interface
    // underneath. A runtime old enough to lack it is left alone rather than
    // wrapped with something that would forward into a null pointer.
    auto* base = static_cast<ID3D12PipelineLibrary*>(*ppv);
    ID3D12PipelineLibrary1* real1 = nullptr;
    if (FAILED(base->QueryInterface(IID_PPV_ARGS(&real1))) || !real1) {
        ProxyLog("[dxr-tier-11-proxy-log] pipeline library has no ID3D12PipelineLibrary1; "
                 "left unwrapped, so a lowered RayQuery pipeline could still be handed "
                 "to StorePipeline\n");
        return;
    }
    base->Release();          // the wrapper owns real1 from here
    *ppv = new Dxr11PipelineLibrary(real1);
    ProxyLog("[dxr-tier-11-proxy-log] pipeline library wrapped, so StorePipeline can "
             "decline a lowered RayQuery pipeline\n");
}
