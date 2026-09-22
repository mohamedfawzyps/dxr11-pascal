// Phase 3b - ID3D12Device5 wrapper implementation.
//
// Every method here is a straight forward to the real device. The only methods
// with any logic of their own are QueryInterface, AddRef and Release.
//
// Keep it that way until Phase 4. When behaviour does get added, it belongs in
// the handful of methods named in d3d12_device.h, and each one should say why
// it diverges.

#include "d3d12_device.h"
#include "pso_stream.h"
#include "shader_dump.h"
#include "proxy_log.h"
#include "state_object_cache.h"
#include "queue_hook.h"
#include "d3d12_command_list.h"
#include "command_signature.h"
#include "dxil_scan.h"
#include "rq_pipeline.h"
#include "res_tracker.h"
#include "config.h"
#include "rewriter/dxc_host.h"

#include <windows.h>
#include <new>
#include <string>

// FWD(method, args...) - forward to the real device, returning its result.
#define FWD(call) return m_real->call

// --- RayQuery detection ----------------------------------------------------
//
// Groundwork for running the rewriter in the proxy, and useful on its own. The
// brief requires that a RayQuery shader be detected and reported clearly
// rather than handed to a driver that cannot run it.
//
// The proxy still reports Tier 1.0, so a well behaved application should never
// emit RayQuery at all. Seeing this log therefore means one of two things: the
// tier has been flipped and the rewriter is needed, or an application is
// ignoring the reported tier. Both are worth knowing about.
//
// Detection does NOT change what is forwarded. On Tier 1.0 the driver rejects
// these shaders on its own, and replacing its error with ours would hide
// information without adding any. The log is the clarity; the behaviour is
// unchanged until the rewriter can actually do something about it.
static void NoteRayQuery(bool tier11, const char* where, const void* code, SIZE_T size) {
    if (!code || !size || !Dxr11ContainerUsesRayQuery(code, size)) return;
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return;
    ProxyLog("[dxr-tier-11-proxy-log] %s: shader USES RAYQUERY (SFI0 bit 20), %zu bytes.\n",
             where, (size_t)size);
    ProxyLog("[dxr-tier-11-proxy-log]   tier reported to the app is %s. On Tier 1.0 with "
             "DXR_TIER11=1 this is lowered and run; otherwise it is "
             "forwarded unchanged and the driver decides.\n",
             tier11 ? "1.1" : "1.0");
}

// A pipeline state stream is a packed sequence of subobjects, each a type enum
// followed by its payload, with every entry aligned to a pointer. Walk it far
// enough to find a compute shader; anything unrecognised ends the walk, since
// the size of an unknown payload is unknowable.
// Every shader a pipeline stream carries, reported. See proxy/pso_stream.h
// for why the previous version of this saw none of them.
static psostream::Parsed NoteRayQueryInStream(bool tier11,
                                              const D3D12_PIPELINE_STATE_STREAM_DESC* d) {
    const psostream::Parsed ps = psostream::Walk(d);
    for (int i = 0; i < ps.shaderCount; ++i)
        NoteRayQuery(tier11, "CreatePipelineState",
                     ps.shaders[i].pShaderBytecode,
                     (SIZE_T)ps.shaders[i].BytecodeLength);

    // A stream shape we cannot walk is worth a line, because the consequence
    // of walking it wrong is a RayQuery shader slipping past unseen, and that
    // surfaces much later as the driver rejecting a shader nobody logged.
    if (!ps.complete)
        ProxyLog("[dxr-tier-11-proxy-log] CreatePipelineState: stopped reading the stream at "
                 "%s (%u). Any shader after that point was not examined.\n",
                 psostream::TypeName(ps.stoppedAt), ps.stoppedAt);
    return ps;
}

// Does the real device offer ID3D12Device<n>? One place, so the startup log
// and QueryInterface cannot disagree.
bool Dxr11Device::Highest(int n) const {
    switch (n) {
        case 6:  return m_real6 != nullptr;
        case 7:  return m_real7 != nullptr;
        case 8: return m_real8 != nullptr;
        case 9: return m_real9 != nullptr;
        case 10: return m_real10 != nullptr;
        case 11: return m_real11 != nullptr;
        case 12: return m_real12 != nullptr;
        case 13: return m_real13 != nullptr;
        case 14: return m_real14 != nullptr;
        case 15: return m_real15 != nullptr;
        default: return false;
    }
}

Dxr11Device::Dxr11Device(ID3D12Device5* real)
    : m_real(real), m_tier11(false), m_real6(nullptr), m_real7(nullptr),
      m_real8(nullptr), m_real9(nullptr), m_real10(nullptr), m_real11(nullptr), m_real12(nullptr), m_real13(nullptr), m_real14(nullptr), m_real15(nullptr), m_refs(1) {
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
        m_real->QueryInterface(__uuidof(ID3D12Device8), (void**)&m_real8);
        m_real->QueryInterface(__uuidof(ID3D12Device9), (void**)&m_real9);
        m_real->QueryInterface(__uuidof(ID3D12Device10), (void**)&m_real10);
        m_real->QueryInterface(__uuidof(ID3D12Device11), (void**)&m_real11);
        m_real->QueryInterface(__uuidof(ID3D12Device12), (void**)&m_real12);
        m_real->QueryInterface(__uuidof(ID3D12Device13), (void**)&m_real13);
        m_real->QueryInterface(__uuidof(ID3D12Device14), (void**)&m_real14);
        m_real->QueryInterface(__uuidof(ID3D12Device15), (void**)&m_real15);
    }
    ProxyLog("[dxr-tier-11-proxy-log] device wrapper created (real=%p, Device6=%s, Device7=%s, tier=%s)\n",
             (void*)m_real, m_real6 ? "yes" : "no", m_real7 ? "yes" : "no",
             m_tier11 ? "1.1" : "1.0");
    // The highest device interface the real device offers. Unreal asks for
    // Device12; anything we do not implement is handed over UNWRAPPED and the
    // application escapes the shim entirely, which is how the first real test
    // of this project failed. Logged so the next ceiling is visible before it
    // costs a day.
    int top = 5;
    for (int n = 6; n <= 15; ++n) if (Highest(n)) top = n;
    ProxyLog("[dxr-tier-11-proxy-log] highest device interface available: ID3D12Device%d "
             "(this shim implements up to 15)\n", top);
    LogCapabilities();
}

// What the real device can do, on the axes that decide whether an engine will
// USE ray tracing once we have told it the tier is 1.1.
//
// This exists because of a real dead end. A run produced a clean log, the tier
// was reported, and nothing happened: no acceleration structure, no state
// object, no RayQuery shader, for sixteen minutes. The tier is necessary and
// it is not sufficient, and from outside there was no way to tell which of the
// OTHER gates had closed.
//
// The gates are Unreal Engine 5's, read out of
// Engine/Source/Runtime/D3D12RHI/Private/Windows/WindowsD3D12Device.cpp
// (FindMaxRHIFeatureLevel) and D3D12Adapter.cpp (the ray tracing block). They
// are not ours to satisfy, and that is the point: when this line says PASSES
// and ray tracing still does not happen, the cause is in the application or
// its content, not in the shim. Other engines gate differently, so this is
// reported as facts plus one engine's verdict, not as a pass or fail.
void Dxr11Device::LogCapabilities() {
    if (!m_real) return;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
                                   D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                   D3D_FEATURE_LEVEL_11_0 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { _countof(levels), levels, D3D_FEATURE_LEVEL_11_0 };
    m_real->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));

    // Probed downwards, because asking for a model the device does not have
    // returns E_INVALIDARG rather than a lower answer. This is how the runtime
    // documents it and how Unreal does it.
    D3D_SHADER_MODEL want[] = { D3D_SHADER_MODEL(0x69), D3D_SHADER_MODEL(0x68),
                                D3D_SHADER_MODEL(0x67), D3D_SHADER_MODEL(0x66),
                                D3D_SHADER_MODEL(0x65), D3D_SHADER_MODEL(0x60) };
    D3D_SHADER_MODEL sm = D3D_SHADER_MODEL(0);
    for (D3D_SHADER_MODEL s : want) {
        D3D12_FEATURE_DATA_SHADER_MODEL q = { s };
        if (SUCCEEDED(m_real->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &q, sizeof(q)))) {
            sm = q.HighestShaderModel;
            break;
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS o0 = {};
    m_real->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o0, sizeof(o0));
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {};
    m_real->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1));
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9 = {};
    const bool have9 = SUCCEEDED(
        m_real->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof(o9)));
    const bool atomic64 = have9 && o9.AtomicInt64OnTypedResourceSupported;

    ProxyLog("[dxr-tier-11-proxy-log]   caps: feature level %x, shader model %d.%d, "
             "resource binding tier %d, wave ops %s, 64-bit typed atomics %s\n",
             (unsigned)fl.MaxSupportedFeatureLevel, (sm >> 4) & 0xf, sm & 0xf,
             (int)o0.ResourceBindingTier, o1.WaveOps ? "yes" : "NO",
             have9 ? (atomic64 ? "yes" : "NO") : "not reported");

    // Unreal needs ALL of these for the SM6 shader platform, and ray tracing is
    // refused outright on SM5 whatever the tier says.
    const bool sm6 = fl.MaxSupportedFeatureLevel >= D3D_FEATURE_LEVEL_12_0 &&
                     sm >= D3D_SHADER_MODEL(0x66) &&
                     o0.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3 &&
                     o1.WaveOps && atomic64;
    if (sm6)
        ProxyLog("[dxr-tier-11-proxy-log]   caps: this device meets Unreal's SM6 bar, so a "
                 "Tier 1.1 answer is enough for it to enable ray tracing. If it still does "
                 "not, the reason is in the application or its settings, not here.\n");
    else
        ProxyLog("[dxr-tier-11-proxy-log]   caps: this device does NOT meet Unreal's SM6 bar, "
                 "which needs feature level 12_0, shader model 6.6, resource binding tier 3, "
                 "wave ops and 64-bit typed atomics. Unreal refuses ray tracing on SM5 "
                 "whatever tier it is told, and no shim can change that.\n");
}

Dxr11Device::~Dxr11Device() {
    if (m_real15) m_real15->Release();
    if (m_real14) m_real14->Release();
    if (m_real13) m_real13->Release();
    if (m_real12) m_real12->Release();
    if (m_real11) m_real11->Release();
    if (m_real10) m_real10->Release();
    if (m_real9) m_real9->Release();
    if (m_real8) m_real8->Release();
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
        (riid == __uuidof(ID3D12Device7) && m_real7) ||
        (riid == __uuidof(ID3D12Device8) && m_real8) ||
        (riid == __uuidof(ID3D12Device9) && m_real9) ||
        (riid == __uuidof(ID3D12Device10) && m_real10) ||
        (riid == __uuidof(ID3D12Device11) && m_real11) ||
        (riid == __uuidof(ID3D12Device12) && m_real12) ||
        (riid == __uuidof(ID3D12Device13) && m_real13) ||
        (riid == __uuidof(ID3D12Device14) && m_real14) ||
        (riid == __uuidof(ID3D12Device15) && m_real15)) {
        AddRef();
        *ppvObject = static_cast<ID3D12Device15*>(this);
        return S_OK;
    }

    // Asked for a higher device interface than the real device has. Say so
    // rather than handing back a pointer whose vtable it cannot honour.
    if (riid == __uuidof(ID3D12Device6) || riid == __uuidof(ID3D12Device7) ||
        riid == __uuidof(ID3D12Device8) ||
        riid == __uuidof(ID3D12Device9) ||
        riid == __uuidof(ID3D12Device10) ||
        riid == __uuidof(ID3D12Device11) ||
        riid == __uuidof(ID3D12Device12) ||
        riid == __uuidof(ID3D12Device13) ||
        riid == __uuidof(ID3D12Device14) ||
        riid == __uuidof(ID3D12Device15))
        return E_NOINTERFACE;

    HRESULT hr = m_real->QueryInterface(riid, ppvObject);
    if (SUCCEEDED(hr)) {
        ProxyLog("[dxr-tier-11-proxy-log] device QI PASSED THROUGH UNWRAPPED: %s\n",
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
        ProxyLog("[dxr-tier-11-proxy-log] device wrapper destroyed (real=%p)\n", (void*)m_real);
        delete this;
    }
    return (ULONG)n;
}

// Wrap a freshly created command list. Only meaningful once the queue hook is
// live, because a wrapper that reached a real ExecuteCommandLists would be
// rejected; if the hook is not active we hand back the real list unchanged
// rather than create something that cannot be submitted.
static HRESULT WrapList(REFIID riid, void** pp) {
    if (!Dxr11QueueHookActive()) return S_OK;
    ID3D12GraphicsCommandList4* real4 = nullptr;
    if (FAILED(static_cast<IUnknown*>(*pp)->QueryInterface(
            __uuidof(ID3D12GraphicsCommandList4), (void**)&real4)) || !real4) {
        return S_OK;   // not a graphics list (a bundle of another type, say)
    }
    auto* wrapper = new (std::nothrow) Dxr11CommandList(real4);   // takes real4's ref
    if (!wrapper) { real4->Release(); return S_OK; }
    void* out = nullptr;
    if (SUCCEEDED(wrapper->QueryInterface(riid, &out)) && out) {
        static_cast<IUnknown*>(*pp)->Release();
        *pp = out;
    }
    wrapper->Release();
    return S_OK;
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
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid, void** ppPipelineState) {
    // The main path: RayQuery lives in compute shaders far more often than
    // anywhere else, because that is where a DXR 1.1 engine puts it.
    if (pDesc)
        NoteRayQuery(m_tier11, "CreateComputePipelineState",
                     pDesc->CS.pShaderBytecode, (SIZE_T)pDesc->CS.BytecodeLength);

    // A RayQuery compute shader cannot run here as a compute shader at all, so
    // lower it and hand back a stand-in that carries the state object and
    // shader table instead. On Tier 1.1 hardware there is nothing to do.
    if (!m_tier11 && pDesc && pDesc->CS.pShaderBytecode &&
        Dxr11ContainerUsesRayQuery(pDesc->CS.pShaderBytecode,
                                   (SIZE_T)pDesc->CS.BytecodeLength) &&
        ppPipelineState && riid == __uuidof(ID3D12PipelineState)) {
        std::string why;
        if (auto* pso = Dxr11RayQueryPso::TryCreate(m_real, pDesc, &why)) {
            *ppPipelineState = pso;
            return S_OK;
        }
        // Refusing is not failing. Forward the original and let the driver
        // give the application its own error, with our reason in the log.
        ProxyLog("[dxr-tier-11-proxy-log] RayQuery compute shader NOT lowered: %s\n"
                 "[dxr-tier-11-proxy-log]   forwarding unchanged; the driver will reject it\n",
                 why.c_str());
        shdump::Refused(pDesc->CS.pShaderBytecode,
                        (size_t)pDesc->CS.BytecodeLength, why.c_str());
    }
    FWD(CreateComputePipelineState(pDesc, riid, ppPipelineState));
}
// Wrapped, not hooked. Hooking a command list does not work: it swaps to a
// per-object heap vtable, so the shared image vtable present here is abandoned
// before the app records anything, and a hook installed here never fires.
// Measured with tier11probe.exe -hooktest. Wrapping is safe because nothing
// outside D3D12 holds a command list; the one leak point,
// ExecuteCommandLists, is covered by the queue vtable hook.
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* pCommandAllocator, ID3D12PipelineState* pInitialState, REFIID riid, void** ppCommandList) {
    HRESULT hr = m_real->CreateCommandList(nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);
    if (!m_tier11 && SUCCEEDED(hr) && ppCommandList && *ppCommandList) WrapList(riid, ppCommandList);
    return hr;
}
// The tier flip, OPT-IN. Reporting Tier 1.1 entitles an application to emit
// RayQuery, and the brief is explicit that a shim which claims 1.1 and then
// fails is worse than one that claims 1.0.
//
// It is nonetheless ON by default, because the opt-in already happened: a
// proxy DLL only exists beside an executable because somebody deliberately put
// it there, and that is the consent. Asking for a second one, through an
// environment variable a launcher never passes on, mostly produced reports
// that the shim does nothing.
//
// Turning it off is still one step, and either of two: delete the DLL, or set
// DXR_TIER11=0. See proxy/config.h for where a value comes from.
static cfg::Flag Tier11Requested() {
    static const cfg::Flag f = cfg::Get("DXR_TIER11", "tier11", true);
    return f;
}

// Claiming Tier 1.1 is a promise to translate RayQuery, and that promise
// cannot be kept without DXC: the rewriter needs dxcompiler.dll to convert a
// container to text and back, and dxil.dll to sign the result.
//
// So the claim is conditional on them being there. Without this, copying the
// shim next to an application and forgetting the other two files produces the
// one failure this project refuses to ship: the application is told it may
// emit RayQuery, does so, and the driver rejects a shader nobody could rewrite.
// The application sees a crash; the log is the only place the real cause
// appears, and by then it has already happened.
//
// Checking here forces DXC to load earlier than it otherwise would, which is
// the point. Better to find out at the first feature query than at the first
// shader.
static bool CanHonourTier11() {
    static const bool ok = [] {
        std::string why;
        if (dxch::Available(&why)) return true;
        ProxyLog("[dxr-tier-11-proxy-log] NOT reporting Tier 1.1: %s. Tier 1.0 is reported "
                 "instead, which is honest, and the application will simply not "
                 "use inline ray tracing. Put dxcompiler.dll and dxil.dll next to "
                 "d3d12.dll to enable it.\n", why.c_str());
        return false;
    }();
    return ok;
}

HRESULT STDMETHODCALLTYPE Dxr11Device::CheckFeatureSupport(D3D12_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) {
    const HRESULT hr = m_real->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
    if (SUCCEEDED(hr) && Tier11Requested().value && !m_tier11 &&
        Feature == D3D12_FEATURE_D3D12_OPTIONS5 &&
        FeatureSupportDataSize >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5)) {
        auto* o5 = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5*>(pFeatureSupportData);
        if (o5->RaytracingTier == D3D12_RAYTRACING_TIER_1_0 && CanHonourTier11()) {
            o5->RaytracingTier = D3D12_RAYTRACING_TIER_1_1;
            static LONG once = 0;
            if (InterlockedCompareExchange(&once, 1, 0) == 0)
                ProxyLog("[dxr-tier-11-proxy-log] reporting Tier 1.1 to the application "
                         "(tier11 on, from %s). RayQuery shaders will be "
                         "rewritten; anything the rewriter refuses is logged and "
                         "forwarded, and the driver then rejects it.\n",
                         Tier11Requested().source);
        }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc, REFIID riid, void** ppvHeap) { FWD(CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap)); }
UINT STDMETHODCALLTYPE Dxr11Device::GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapType) { FWD(GetDescriptorHandleIncrementSize(DescriptorHeapType)); }
// Remembers the blob when shader dumping is on, so a lowered library can be
// replayed offline against the root signature it was actually built with.
// D3D12 gives no way back from the object to the blob. See shader_dump.h.
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateRootSignature(UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes, REFIID riid, void** ppvRootSignature) {
    const HRESULT hr = m_real->CreateRootSignature(nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
    if (SUCCEEDED(hr) && ppvRootSignature && *ppvRootSignature)
        shdump::NoteRootSignature(*ppvRootSignature, pBlobWithRootSignature,
                                  static_cast<size_t>(blobLengthInBytes));
    return hr;
}
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
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource) {
    const HRESULT hr = m_real->CreateCommittedResource(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidResource, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
        ID3D12Resource* res = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppvResource)->QueryInterface(IID_PPV_ARGS(&res)))) {
            restrack::Note(res);
            res->Release();   // the tracker holds no reference
        }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateHeap(const D3D12_HEAP_DESC* pDesc, REFIID riid, void** ppvHeap) { FWD(CreateHeap(pDesc, riid, ppvHeap)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreatePlacedResource(ID3D12Heap* pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    const HRESULT hr = m_real->CreatePlacedResource(pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
        ID3D12Resource* res = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppvResource)->QueryInterface(IID_PPV_ARGS(&res)))) {
            restrack::Note(res);
            res->Release();   // the tracker holds no reference
        }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateReservedResource(const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource) {
    const HRESULT hr = m_real->CreateReservedResource(pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
        ID3D12Resource* res = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppvResource)->QueryInterface(IID_PPV_ARGS(&res)))) {
            restrack::Note(res);
            res->Release();   // the tracker holds no reference
        }
    }
    return hr;
}
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
// Tier 1.0 refuses a DISPATCH_RAYS signature outright, so the app can never
// reach ExecuteIndirect. Hand back a stand-in instead, which ExecuteIndirect
// recognises and services itself. See command_signature.h.
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* pDesc, ID3D12RootSignature* pRootSignature, REFIID riid, void** ppvCommandSignature) {
    bool dispatchRays = false;
    if (!m_tier11 && pDesc) {
        for (UINT i = 0; i < pDesc->NumArgumentDescs; ++i)
            if (pDesc->pArgumentDescs[i].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS)
                dispatchRays = true;
    }
    if (!dispatchRays) FWD(CreateCommandSignature(pDesc, pRootSignature, riid, ppvCommandSignature));

    // D3D12 requires DISPATCH_RAYS to be the only argument in its signature.
    // Anything else is a shape we have not seen and cannot emulate, so refuse
    // rather than guess.
    if (pDesc->NumArgumentDescs != 1) {
        ProxyLog("[dxr-tier-11-proxy-log] CreateCommandSignature: DISPATCH_RAYS alongside %u other "
                 "arguments is not supported\n", pDesc->NumArgumentDescs - 1);
        return E_INVALIDARG;
    }
    if (!ppvCommandSignature) return E_INVALIDARG;

    auto* sig = new (std::nothrow) Dxr11CommandSignature(m_real, *pDesc);
    if (!sig) return E_OUTOFMEMORY;
    HRESULT hr = sig->QueryInterface(riid, ppvCommandSignature);
    sig->Release();
    ProxyLog("[dxr-tier-11-proxy-log] CreateCommandSignature: DISPATCH_RAYS stand-in created "
             "(ByteStride=%u) hr=0x%08lx\n", pDesc->ByteStride, (unsigned long)hr);
    return hr;
}
void STDMETHODCALLTYPE Dxr11Device::GetResourceTiling(ID3D12Resource* pTiledResource, UINT* pNumTilesForEntireResource, D3D12_PACKED_MIP_INFO* pPackedMipDesc, D3D12_TILE_SHAPE* pStandardTileShapeForNonPackedMips, UINT* pNumSubresourceTilings, UINT FirstSubresourceTilingToGet, D3D12_SUBRESOURCE_TILING* pSubresourceTilingsForNonPackedMips) { FWD(GetResourceTiling(pTiledResource, pNumTilesForEntireResource, pPackedMipDesc, pStandardTileShapeForNonPackedMips, pNumSubresourceTilings, FirstSubresourceTilingToGet, pSubresourceTilingsForNonPackedMips)); }
LUID STDMETHODCALLTYPE Dxr11Device::GetAdapterLuid() { FWD(GetAdapterLuid()); }

// --- ID3D12Device1 ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::CreatePipelineLibrary(const void* pLibraryBlob, SIZE_T BlobLength, REFIID riid, void** ppPipelineLibrary) { FWD(CreatePipelineLibrary(pLibraryBlob, BlobLength, riid, ppPipelineLibrary)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::SetEventOnMultipleFenceCompletion(ID3D12Fence* const* ppFences, const UINT64* pFenceValues, UINT NumFences, D3D12_MULTIPLE_FENCE_WAIT_FLAGS Flags, HANDLE hEvent) { FWD(SetEventOnMultipleFenceCompletion(ppFences, pFenceValues, NumFences, Flags, hEvent)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::SetResidencyPriority(UINT NumObjects, ID3D12Pageable* const* ppObjects, const D3D12_RESIDENCY_PRIORITY* pPriorities) { FWD(SetResidencyPriority(NumObjects, ppObjects, pPriorities)); }

// --- ID3D12Device2 ----------------------------------------------------------

// The stream form of the same thing CreateComputePipelineState does, and it
// needs the same substitution. An engine with a unified PSO cache creates
// EVERYTHING here, compute included; Unreal does, which is why this path being
// detection-only was not a gap in coverage but the whole of it.
HRESULT STDMETHODCALLTYPE Dxr11Device::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, REFIID riid, void** ppPipelineState) {
    const psostream::Parsed ps = NoteRayQueryInStream(m_tier11, pDesc);

    // Compute only, never a stream that also carries a graphics or mesh stage.
    // RayQuery in a pixel shader has no lowering, and substituting a stand-in
    // for a graphics pipeline would be far worse than forwarding it.
    if (!m_tier11 && ps.complete && ps.IsComputeOnly() &&
        Dxr11ContainerUsesRayQuery(ps.cs.pShaderBytecode, (SIZE_T)ps.cs.BytecodeLength) &&
        ppPipelineState && riid == __uuidof(ID3D12PipelineState)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature = ps.rootSignature;
        cd.CS = ps.cs;
        cd.NodeMask = ps.nodeMask;
        cd.Flags = ps.flags;
        // The cached blob is deliberately NOT carried over. It was produced for
        // the original compute shader, and what gets created here is a state
        // object built from a rewritten library.
        std::string why;
        if (auto* pso = Dxr11RayQueryPso::TryCreate(m_real, &cd, &why)) {
            *ppPipelineState = pso;
            return S_OK;
        }
        ProxyLog("[dxr-tier-11-proxy-log] RayQuery compute shader NOT lowered: %s\n"
                 "[dxr-tier-11-proxy-log]   forwarding unchanged; the driver will reject it\n",
                 why.c_str());
        shdump::Refused(ps.cs.pShaderBytecode, (size_t)ps.cs.BytecodeLength,
                        why.c_str());
    }
    FWD(CreatePipelineState(pDesc, riid, ppPipelineState));
}

// --- ID3D12Device3 ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::OpenExistingHeapFromAddress(const void* pAddress, REFIID riid, void** ppvHeap) { FWD(OpenExistingHeapFromAddress(pAddress, riid, ppvHeap)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::OpenExistingHeapFromFileMapping(HANDLE hFileMapping, REFIID riid, void** ppvHeap) { FWD(OpenExistingHeapFromFileMapping(hFileMapping, riid, ppvHeap)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::EnqueueMakeResident(D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable* const* ppObjects, ID3D12Fence* pFenceToSignal, UINT64 FenceValueToSignal) { FWD(EnqueueMakeResident(Flags, NumObjects, ppObjects, pFenceToSignal, FenceValueToSignal)); }

// --- ID3D12Device4 ----------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommandList1(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** ppCommandList) {
    HRESULT hr = m_real->CreateCommandList1(nodeMask, type, flags, riid, ppCommandList);
    if (!m_tier11 && SUCCEEDED(hr) && ppCommandList && *ppCommandList) WrapList(riid, ppCommandList);
    return hr;
}
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateProtectedResourceSession(const D3D12_PROTECTED_RESOURCE_SESSION_DESC* pDesc, REFIID riid, void** ppSession) { FWD(CreateProtectedResourceSession(pDesc, riid, ppSession)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommittedResource1(const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession, REFIID riidResource, void** ppvResource) {
    const HRESULT hr = m_real->CreateCommittedResource1(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riidResource, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
        ID3D12Resource* res = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppvResource)->QueryInterface(IID_PPV_ARGS(&res)))) {
            restrack::Note(res);
            res->Release();   // the tracker holds no reference
        }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateHeap1(const D3D12_HEAP_DESC* pDesc, ID3D12ProtectedResourceSession* pProtectedSession, REFIID riid, void** ppvHeap) { FWD(CreateHeap1(pDesc, pProtectedSession, riid, ppvHeap)); }
HRESULT STDMETHODCALLTYPE Dxr11Device::CreateReservedResource1(const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, ID3D12ProtectedResourceSession* pProtectedSession, REFIID riid, void** ppvResource) {
    const HRESULT hr = m_real->CreateReservedResource1(pDesc, InitialState, pOptimizedClearValue, pProtectedSession, riid, ppvResource);
    if (SUCCEEDED(hr) && ppvResource && *ppvResource) {
        ID3D12Resource* res = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppvResource)->QueryInterface(IID_PPV_ARGS(&res)))) {
            restrack::Note(res);
            res->Release();   // the tracker holds no reference
        }
    }
    return hr;
}
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
    // RayQuery is legal inside a DXR 1.1 raygen or miss shader too, so the
    // libraries here are worth checking even though compute is the common case.
    if (pDesc && pDesc->pSubobjects) {
        for (UINT i = 0; i < pDesc->NumSubobjects; ++i) {
            const auto& so = pDesc->pSubobjects[i];
            if (so.Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !so.pDesc)
                continue;
            auto* lib = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(so.pDesc);
            NoteRayQuery(m_tier11, "CreateStateObject",
                         lib->DXILLibrary.pShaderBytecode,
                         (SIZE_T)lib->DXILLibrary.BytecodeLength);
        }
    }
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
        ProxyLog("[dxr-tier-11-proxy-log] CreateStateObject: cannot cache subobjects (%s); "
                 "additions will not work for this object\n", why.c_str());
        delete store;
        FWD(CreateStateObject(pDesc, riid, ppStateObject));
    }
    store->StripAdditionsFlag();

    D3D12_STATE_OBJECT_DESC stripped = store->Desc(pDesc->Type);
    HRESULT hr = m_real->CreateStateObject(&stripped, riid, ppStateObject);
    ProxyLog("[dxr-tier-11-proxy-log] CreateStateObject: stripped ALLOW_STATE_OBJECT_ADDITIONS, "
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
        ProxyLog("[dxr-tier-11-proxy-log] AddToStateObject forwarded (tier 1.1) hr=0x%08lx\n",
                 (unsigned long)hr);
        return hr;
    }
    if (!pAddition || !pStateObjectToGrowFrom || !ppNewStateObject) return E_INVALIDARG;

    StateObjectStore* base = StateObjectCacheGet(pStateObjectToGrowFrom);
    if (!base) {
        ProxyLog("[dxr-tier-11-proxy-log] AddToStateObject: no cached subobjects for %p. The "
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
        ProxyLog("[dxr-tier-11-proxy-log] AddToStateObject: merge failed (%s)\n", why.c_str());
        delete merged;
        return E_INVALIDARG;
    }
    merged->StripAdditionsFlag();

    D3D12_STATE_OBJECT_DESC full = merged->Desc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
    HRESULT hr = m_real->CreateStateObject(&full, riid, ppNewStateObject);
    ProxyLog("[dxr-tier-11-proxy-log] AddToStateObject EMULATED: base %u + addition %u -> "
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
        ProxyLog("[dxr-tier-11-proxy-log] no ID3D12Device5 on this device (hr=0x%08lx), not wrapping\n",
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

// --- interfaces above Device7 -----------------------------------------------
//
// All forwarding. They exist so that an application asking for a newer
// device interface still gets the WRAPPER: Unreal asks for Device12, and
// handing over the real device there means every later call, including
// CheckFeatureSupport and every pipeline creation, bypasses this shim.

// --- Dxr11Device ID3D12Device8 -----------------------------------------------

D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE Dxr11Device::GetResourceAllocationInfo2(UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC1 *pResourceDescs, D3D12_RESOURCE_ALLOCATION_INFO1 *pResourceAllocationInfo1) {
    if (!m_real8) return D3D12_RESOURCE_ALLOCATION_INFO{};
    return m_real8->GetResourceAllocationInfo2(visibleMask, numResourceDescs, pResourceDescs, pResourceAllocationInfo1);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommittedResource2(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *pProtectedSession, REFIID riidResource, void **ppvResource) {
    if (!m_real8) return E_NOINTERFACE;
    return m_real8->CreateCommittedResource2(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riidResource, ppvResource);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::CreatePlacedResource1(ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riid, void **ppvResource) {
    if (!m_real8) return E_NOINTERFACE;
    return m_real8->CreatePlacedResource1(pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);
}

void STDMETHODCALLTYPE Dxr11Device::CreateSamplerFeedbackUnorderedAccessView(ID3D12Resource *pTargetedResource, ID3D12Resource *pFeedbackResource, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!m_real8) return;
    return m_real8->CreateSamplerFeedbackUnorderedAccessView(pTargetedResource, pFeedbackResource, DestDescriptor);
}

void STDMETHODCALLTYPE Dxr11Device::GetCopyableFootprints1(const D3D12_RESOURCE_DESC1 *pResourceDesc, UINT FirstSubresource, UINT NumSubresources, UINT64 BaseOffset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *pLayouts, UINT *pNumRows, UINT64 *pRowSizeInBytes, UINT64 *pTotalBytes) {
    if (!m_real8) return;
    return m_real8->GetCopyableFootprints1(pResourceDesc, FirstSubresource, NumSubresources, BaseOffset, pLayouts, pNumRows, pRowSizeInBytes, pTotalBytes);
}



// --- Dxr11Device ID3D12Device9 -----------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateShaderCacheSession(const D3D12_SHADER_CACHE_SESSION_DESC *pDesc, REFIID riid, void **ppvSession) {
    if (!m_real9) return E_NOINTERFACE;
    return m_real9->CreateShaderCacheSession(pDesc, riid, ppvSession);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAGS Kinds, D3D12_SHADER_CACHE_CONTROL_FLAGS Control) {
    if (!m_real9) return E_NOINTERFACE;
    return m_real9->ShaderCacheControl(Kinds, Control);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommandQueue1(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID CreatorID, REFIID riid, void **ppCommandQueue) {
    if (!m_real9) return E_NOINTERFACE;
    return m_real9->CreateCommandQueue1(pDesc, CreatorID, riid, ppCommandQueue);
}



// --- Dxr11Device ID3D12Device10 -----------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateCommittedResource3(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *pProtectedSession, UINT32 NumCastableFormats, const DXGI_FORMAT *pCastableFormats, REFIID riidResource, void **ppvResource) {
    if (!m_real10) return E_NOINTERFACE;
    return m_real10->CreateCommittedResource3(pHeapProperties, HeapFlags, pDesc, InitialLayout, pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats, riidResource, ppvResource);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::CreatePlacedResource2(ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *pOptimizedClearValue, UINT32 NumCastableFormats, const DXGI_FORMAT *pCastableFormats, REFIID riid, void **ppvResource) {
    if (!m_real10) return E_NOINTERFACE;
    return m_real10->CreatePlacedResource2(pHeap, HeapOffset, pDesc, InitialLayout, pOptimizedClearValue, NumCastableFormats, pCastableFormats, riid, ppvResource);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateReservedResource2(const D3D12_RESOURCE_DESC *pDesc, D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *pProtectedSession, UINT32 NumCastableFormats, const DXGI_FORMAT *pCastableFormats, REFIID riid, void **ppvResource) {
    if (!m_real10) return E_NOINTERFACE;
    return m_real10->CreateReservedResource2(pDesc, InitialLayout, pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats, riid, ppvResource);
}



// --- Dxr11Device ID3D12Device11 -----------------------------------------------

void STDMETHODCALLTYPE Dxr11Device::CreateSampler2(const D3D12_SAMPLER_DESC2 *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!m_real11) return;
    return m_real11->CreateSampler2(pDesc, DestDescriptor);
}



// --- Dxr11Device ID3D12Device12 -----------------------------------------------

D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE Dxr11Device::GetResourceAllocationInfo3(UINT visibleMask, UINT numResourceDescs, const D3D12_RESOURCE_DESC1 *pResourceDescs, const UINT32 *pNumCastableFormats, const DXGI_FORMAT *const *ppCastableFormats, D3D12_RESOURCE_ALLOCATION_INFO1 *pResourceAllocationInfo1) {
    if (!m_real12) return D3D12_RESOURCE_ALLOCATION_INFO{};
    return m_real12->GetResourceAllocationInfo3(visibleMask, numResourceDescs, pResourceDescs, pNumCastableFormats, ppCastableFormats, pResourceAllocationInfo1);
}



// --- Dxr11Device ID3D12Device13 -----------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::OpenExistingHeapFromAddress1(const void *pAddress, SIZE_T size, REFIID riid, void **ppvHeap) {
    if (!m_real13) return E_NOINTERFACE;
    return m_real13->OpenExistingHeapFromAddress1(pAddress, size, riid, ppvHeap);
}



// --- Dxr11Device ID3D12Device14 -----------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateRootSignatureFromSubobjectInLibrary(UINT nodeMask, const void *pLibraryBlob, SIZE_T blobLengthInBytes, LPCWSTR subobjectName, REFIID riid, void **ppvRootSignature) {
    if (!m_real14) return E_NOINTERFACE;
    return m_real14->CreateRootSignatureFromSubobjectInLibrary(nodeMask, pLibraryBlob, blobLengthInBytes, subobjectName, riid, ppvRootSignature);
}



// --- Dxr11Device ID3D12Device15 -----------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11Device::RegisterTrimNotificationCallback(D3D12_REGISTER_TRIM_NOTIFICATION *pData) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->RegisterTrimNotificationCallback(pData);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::UnregisterTrimNotificationCallback(DWORD CallbackCookie) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->UnregisterTrimNotificationCallback(CallbackCookie);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::TryCreateShaderResourceView(ID3D12Resource *pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->TryCreateShaderResourceView(pResource, pDesc, DestDescriptor);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::TryCreateUnorderedAccessView(ID3D12Resource *pResource, ID3D12Resource *pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->TryCreateUnorderedAccessView(pResource, pCounterResource, pDesc, DestDescriptor);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::TryCreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->TryCreateConstantBufferView(pDesc, DestDescriptor);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::TryCreateSampler2(const D3D12_SAMPLER_DESC2 *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->TryCreateSampler2(pDesc, DestDescriptor);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::TryCreateRenderTargetView(ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->TryCreateRenderTargetView(pResource, pDesc, DestDescriptor);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::TryCreateDepthStencilView(ID3D12Resource *pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->TryCreateDepthStencilView(pResource, pDesc, DestDescriptor);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::TryCreateSamplerFeedbackUnorderedAccessView(ID3D12Resource *pTargetedResource, ID3D12Resource *pFeedbackResource, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->TryCreateSamplerFeedbackUnorderedAccessView(pTargetedResource, pFeedbackResource, DestDescriptor);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::CreateQueryHeap1(const D3D12_QUERY_HEAP_DESC *pDesc, D3D12_QUERY_HEAP_FLAGS Flags, REFIID riid, void **ppvHeap) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->CreateQueryHeap1(pDesc, Flags, riid, ppvHeap);
}

HRESULT STDMETHODCALLTYPE Dxr11Device::ResolveQueryData(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries, void *pResolvedQueryData) {
    if (!m_real15) return E_NOINTERFACE;
    return m_real15->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries, pResolvedQueryData);
}
