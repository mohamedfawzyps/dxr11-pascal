// Phase 3b - proxy d3d12.dll exports, plus device wrapping.
//
// When this DLL is placed next to an application's exe, Windows loads it
// instead of the system d3d12.dll (the app directory is searched before
// System32). Every export forwards to the real system d3d12.dll. Since 3b,
// D3D12CreateDevice additionally wraps the returned device in Dxr11Device,
// which forwards every method unchanged. Still no translation.
//
// The real runtime is loaded by full System32 path, so forwarding never
// recurses back into this proxy.
//
// Export ORDINALS must match the real d3d12.dll, see d3d12_proxy.def. The SDK
// import library binds D3D12CreateDevice by ordinal 101, not by name.
//
// Build: build_proxy.bat -> d3d12.dll. Copy it next to the target exe.
// Log:   %TEMP%\dxr-tier-11-proxy.log (and OutputDebugString).
//
// Env:   DXR_TIER11_NOWRAP=1  forward only, do not wrap the device. Useful for
//                         bisecting whether a problem is the wrapper or the
//                         proxy itself.

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <string>
#include <cstdarg>
#include <cwchar>

#include "proxy_log.h"
#include "version.h"
#include "config.h"
#include "dllinfo.h"
#include "rewriter/dxc_host.h"
#include "d3d12_device.h"

// --- real system d3d12.dll -------------------------------------------------
static HMODULE RealD3D12() {
    static HMODULE h = [] {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return (HMODULE)nullptr;
        wcscat_s(path, MAX_PATH, L"\\d3d12.dll");
        return LoadLibraryW(path);
    }();
    return h;
}

template <class T>
static T RealProc(const char* name) {
    HMODULE h = RealD3D12();
    return h ? reinterpret_cast<T>(GetProcAddress(h, name)) : nullptr;
}

// --- logging -------------------------------------------------------------
//
// ProxyLog, the log destination and the exe name moved to proxy_log.cpp so
// that dxgi.dll can use them too. Only ProxyIidName is D3D12's and stays.

const char* ProxyIidName(const IID& iid) {
#define IIDCASE(T) if (iid == __uuidof(T)) return #T;
    IIDCASE(IUnknown)
    IIDCASE(ID3D12Object)
    IIDCASE(ID3D12Device)
    IIDCASE(ID3D12Device1)
    IIDCASE(ID3D12Device2)
    IIDCASE(ID3D12Device3)
    IIDCASE(ID3D12Device4)
    IIDCASE(ID3D12Device5)
    // Device6 and up are NOT wrapped. Device7 in particular carries
    // AddToStateObject, a Phase 4 target, so seeing one of these in the log is
    // a signal that the wrapper needs extending before that phase can work.
#ifdef __ID3D12Device6_INTERFACE_DEFINED__
    IIDCASE(ID3D12Device6)
#endif
#ifdef __ID3D12Device7_INTERFACE_DEFINED__
    IIDCASE(ID3D12Device7)
#endif
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
    IIDCASE(ID3D12Device8)
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    IIDCASE(ID3D12Device9)
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    IIDCASE(ID3D12Device10)
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    IIDCASE(ID3D12Device11)
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    IIDCASE(ID3D12Device12)
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    IIDCASE(ID3D12Device13)
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    IIDCASE(ID3D12Device14)
#endif
#ifdef __ID3D12GraphicsCommandList_INTERFACE_DEFINED__
    IIDCASE(ID3D12GraphicsCommandList)
    IIDCASE(ID3D12GraphicsCommandList1)
    IIDCASE(ID3D12GraphicsCommandList2)
    IIDCASE(ID3D12GraphicsCommandList3)
    IIDCASE(ID3D12GraphicsCommandList4)
#endif
#ifdef __ID3D12GraphicsCommandList5_INTERFACE_DEFINED__
    IIDCASE(ID3D12GraphicsCommandList5)
#endif
#ifdef __ID3D12GraphicsCommandList6_INTERFACE_DEFINED__
    IIDCASE(ID3D12GraphicsCommandList6)
#endif
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
    IIDCASE(ID3D12GraphicsCommandList7)
#endif
    IIDCASE(ID3D12DebugDevice)
#ifdef __ID3D12DebugDevice1_INTERFACE_DEFINED__
    IIDCASE(ID3D12DebugDevice1)
#endif
#ifdef __ID3D12DebugDevice2_INTERFACE_DEFINED__
    IIDCASE(ID3D12DebugDevice2)
#endif
#ifdef __ID3D12InfoQueue_INTERFACE_DEFINED__
    IIDCASE(ID3D12InfoQueue)
#endif
#ifdef __ID3D12InfoQueue1_INTERFACE_DEFINED__
    IIDCASE(ID3D12InfoQueue1)
#endif
#undef IIDCASE
    static char buf[64];
    std::snprintf(buf, sizeof(buf),
        "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        (unsigned long)iid.Data1, iid.Data2, iid.Data3,
        iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
        iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
    return buf;
}

static bool WrapEnabled() {
    static bool on = [] {
        const cfg::Flag f = cfg::Get("DXR_TIER11_NOWRAP", "nowrap", false);
        if (f.value) {
            ProxyLog("[dxr-tier-11-proxy-log] device wrapping DISABLED (nowrap on, from %s). "
                     "Nothing is translated, including the Tier 1.1 answer.\n",
                     f.source);
            return false;
        }

        // Without DXC there is nothing left to do, so do nothing at all.
        //
        // Everything the wrapper still handles is a Tier 1.1 feature:
        // AddToStateObject, indirect DispatchRays, and the command list and
        // acceleration structure machinery those need. Without DXC the shim
        // reports Tier 1.0, and an application that reads the tier it was
        // given never calls any of them. The wrapper would sit in the path of
        // every device and command list call waiting for work that cannot
        // arrive.
        //
        // That is risk with no benefit, and the risk is not theoretical: the
        // wrapper hooks a queue vtable and wraps every command list. Standing
        // aside is one less thing that can be wrong in somebody's game.
        //
        // The log survives: the lines that say what happened come from this
        // file, not from the wrapper.
        std::string why;
        if (!dxch::Available(&why)) {
            ProxyLog("[dxr-tier-11-proxy-log] standing aside entirely: %s. The "
                     "application gets the real device untouched, which is what "
                     "it would have had with no shim installed at all.\n",
                     why.c_str());
            return false;
        }

        ProxyLog("[dxr-tier-11-proxy-log] device wrapping enabled\n");
        return true;
    }();
    return on;
}

// --- what is actually loaded ------------------------------------------------
//
// Four DLLs that are not ours decide whether a run works, and the versions
// matter: DXC is what converts and signs the rewritten shader, and the
// application's own Agility SDK is what interprets the result. Reported once,
// from the first device creation, because that is the earliest point at which
// all four have had their chance to load. DllMain is too early: the runtime
// loads D3D12Core.dll during D3D12CreateDevice.
static void ReportEnvironmentOnce() {
    static bool done = [] {
        // Probing DXC here rather than waiting for the first shader, so the
        // log answers "is the rewriter usable at all" on every run rather than
        // only on runs that got as far as a RayQuery shader. Idempotent: the
        // wrap decision below consults the same cached result.
        std::string why;
        if (!dxch::Available(&why))
            ProxyLog("[dxr-tier-11-proxy-log] DXC:     NOT AVAILABLE, %s\n", why.c_str());
        else
            ProxyLog("[dxr-tier-11-proxy-log] DXC:     %s\n", dxch::Versions());

        // The Agility SDK the APPLICATION ships, if it ships one. Unreal does,
        // and it is a common enough cause of a shim behaving differently in
        // two games that it belongs in every log rather than in a follow-up
        // question.
        const std::string core = dllinfo::OfLoadedModule(L"D3D12Core.dll");
        ProxyLog("[dxr-tier-11-proxy-log] runtime: D3D12Core.dll %s%s\n", core.c_str(),
                 core == "not loaded"
                     ? ", so this application uses the operating system's own D3D12"
                     : "");

        const std::string layers = dllinfo::OfLoadedModule(L"d3d12SDKLayers.dll");
        ProxyLog("[dxr-tier-11-proxy-log] runtime: d3d12SDKLayers.dll %s%s\n", layers.c_str(),
                 layers == "not loaded" ? ", so the debug layer is off, which is normal" : "");
        // By HANDLE, not by name: GetModuleHandleW(L"d3d12.dll") inside a proxy
        // called d3d12.dll answers with the proxy.
        ProxyLog("[dxr-tier-11-proxy-log] runtime: the real d3d12.dll %s\n",
                 dllinfo::OfModule(RealD3D12()).c_str());
        return true;
    }();
    (void)done;
}

// --- undocumented exports, forwarded by tail-jump thunk ---------------------
//
// d3d12SDKLayers.dll imports these three from "d3d12.dll" by name. Since our
// proxy is the module called d3d12.dll in the app directory, it has to provide
// them or the debug layer cannot load, and D3D12Core reports the resulting
// LoadLibrary failure as the rather misleading
// "D3D12 SDKLayers dll not found at D3D12SDKPath" (0x887E0003).
//
// Their signatures are not public, so they are forwarded in assembly, where a
// tail jump needs no signature at all. See proxy\d3d12_thunks.asm.
// Index order must match the THUNK list at the bottom of that file.
static const char* const g_thunkNames[] = {
    "D3D12CoreCreateLayeredDevice",
    "D3D12CoreGetLayeredDeviceSize",
    "D3D12CoreRegisterLayers",
};

extern "C" void* Dxr11ResolveThunk(unsigned index) {
    static void* cache[_countof(g_thunkNames)] = {};
    if (index >= _countof(g_thunkNames)) return nullptr;
    if (!cache[index]) {
        HMODULE h = RealD3D12();
        cache[index] = h ? (void*)GetProcAddress(h, g_thunkNames[index]) : nullptr;
        ProxyLog("[dxr-tier-11-proxy-log] thunk %s -> %p\n", g_thunkNames[index], cache[index]);
    }
    return cache[index];
}

// --- forwarded exports -----------------------------------------------------
typedef HRESULT (WINAPI *PFN_CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT (WINAPI *PFN_GetDebugInterface)(REFIID, void**);
typedef HRESULT (WINAPI *PFN_SerializeRS)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI *PFN_SerializeVRS)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
typedef HRESULT (WINAPI *PFN_CreateRSDeserializer)(LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT (WINAPI *PFN_CreateVRSDeserializer)(LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT (WINAPI *PFN_EnableExperimental)(UINT, const IID*, void*, UINT*);
typedef HRESULT (WINAPI *PFN_GetInterface)(REFCLSID, REFIID, void**);

extern "C" HRESULT WINAPI D3D12CreateDevice(
        IUnknown* pAdapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppDevice) {
    static PFN_CreateDevice real = RealProc<PFN_CreateDevice>("D3D12CreateDevice");
    if (!real) return E_FAIL;

    HRESULT hr = real(pAdapter, fl, riid, ppDevice);

    // After the real call, so D3D12Core.dll has had its chance to load.
    ReportEnvironmentOnce();

    // ppDevice == nullptr is the documented capability-probe form: it answers
    // "could this adapter make this device" and returns S_FALSE without
    // producing an object. Nothing to wrap.
    if (!ppDevice) {
        ProxyLog("[dxr-tier-11-proxy-log] D3D12CreateDevice fl=0x%x hr=0x%08lx (capability probe)\n",
                 (unsigned)fl, (unsigned long)hr);
        return hr;
    }

    if (FAILED(hr) || !*ppDevice) {
        ProxyLog("[dxr-tier-11-proxy-log] D3D12CreateDevice fl=0x%x hr=0x%08lx device=null\n",
                 (unsigned)fl, (unsigned long)hr);
        return hr;
    }

    void* realDevice = *ppDevice;

    if (WrapEnabled()) {
        void* wrapped = nullptr;
        HRESULT whr = Dxr11WrapDevice(static_cast<IUnknown*>(realDevice), riid, &wrapped);
        if (SUCCEEDED(whr) && wrapped) {
            // The wrapper owns the device now; drop the reference the runtime
            // handed us for the caller.
            static_cast<IUnknown*>(realDevice)->Release();
            *ppDevice = wrapped;
            ProxyLog("[dxr-tier-11-proxy-log] D3D12CreateDevice fl=0x%x hr=0x%08lx real=%p wrapped=%p as %s\n",
                     (unsigned)fl, (unsigned long)hr, realDevice, wrapped, ProxyIidName(riid));
            return hr;
        }
        ProxyLog("[dxr-tier-11-proxy-log] D3D12CreateDevice fl=0x%x hr=0x%08lx real=%p NOT wrapped (0x%08lx)\n",
                 (unsigned)fl, (unsigned long)hr, realDevice, (unsigned long)whr);
        return hr;
    }

    ProxyLog("[dxr-tier-11-proxy-log] D3D12CreateDevice fl=0x%x hr=0x%08lx device=%p (wrapping off)\n",
             (unsigned)fl, (unsigned long)hr, realDevice);
    return hr;
}

extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** ppvDebug) {
    static PFN_GetDebugInterface real = RealProc<PFN_GetDebugInterface>("D3D12GetDebugInterface");
    return real ? real(riid, ppvDebug) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(
        const D3D12_ROOT_SIGNATURE_DESC* p, D3D_ROOT_SIGNATURE_VERSION v,
        ID3DBlob** ppBlob, ID3DBlob** ppErr) {
    static PFN_SerializeRS real = RealProc<PFN_SerializeRS>("D3D12SerializeRootSignature");
    return real ? real(p, v, ppBlob, ppErr) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* p, ID3DBlob** ppBlob, ID3DBlob** ppErr) {
    static PFN_SerializeVRS real = RealProc<PFN_SerializeVRS>("D3D12SerializeVersionedRootSignature");
    return real ? real(p, ppBlob, ppErr) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
        LPCVOID pData, SIZE_T size, REFIID riid, void** pp) {
    static PFN_CreateRSDeserializer real =
        RealProc<PFN_CreateRSDeserializer>("D3D12CreateRootSignatureDeserializer");
    return real ? real(pData, size, riid, pp) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
        LPCVOID pData, SIZE_T size, REFIID riid, void** pp) {
    static PFN_CreateVRSDeserializer real =
        RealProc<PFN_CreateVRSDeserializer>("D3D12CreateVersionedRootSignatureDeserializer");
    return real ? real(pData, size, riid, pp) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(
        UINT num, const IID* pIIDs, void* pCfg, UINT* pSizes) {
    static PFN_EnableExperimental real =
        RealProc<PFN_EnableExperimental>("D3D12EnableExperimentalFeatures");
    return real ? real(num, pIIDs, pCfg, pSizes) : E_NOTIMPL;
}

extern "C" HRESULT WINAPI D3D12GetInterface(REFCLSID rclsid, REFIID riid, void** pp) {
    static PFN_GetInterface real = RealProc<PFN_GetInterface>("D3D12GetInterface");
    return real ? real(rclsid, riid, pp) : E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Remember our own module so the rewriter can load the DXC beside US,
        // by full path. A bare LoadLibraryW would hand back the application's
        // own dxcompiler.dll if it has one, of some other version. Nothing is
        // loaded here: the host loads lazily, so an application that never
        // uses RayQuery pays nothing for this.
        dxch::SetHostModule(self);
        ProxyLog("[dxr-tier-11-proxy-log] ======================== start: %s (pid %lu), "
                 "shim " DXR_TIER11_VERSION " ========================\n",
                 ProxyHostExeName(), (unsigned long)GetCurrentProcessId());
    } else if (reason == DLL_PROCESS_DETACH) {
        // A MISSING end marker is itself the finding: it means the process did
        // not unload us, which is what a crash looks like from in here. So the
        // marker is worth having even though it cannot be relied on to appear.
        //
        // Nothing else happens on detach. During process teardown the loader
        // lock is held and other DLLs may already be gone, so this stays to one
        // formatted line and no cleanup.
        ProxyLog("[dxr-tier-11-proxy-log] ========================= end: %s (pid %lu) "
                 "=========================\n",
                 ProxyHostExeName(), (unsigned long)GetCurrentProcessId());
    }
    return TRUE;
}
