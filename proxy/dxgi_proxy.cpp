// Proxy dxgi.dll.
//
// It exists for one reason: Unreal Engine refuses ray tracing on Pascal by PCI
// device id, AFTER accepting the Tier 1.1 answer the d3d12 shim gives it. The
// device id arrives through DXGI, not D3D12, so the d3d12 shim cannot reach it.
// See dxgi_spoof.h for the verified engine source and for what is and is not
// changed.
//
// It also cannot be done from inside d3d12.dll by any other route: Unreal calls
// IDXGIAdapter::GetDesc BEFORE its first D3D12CreateDevice, so there is no
// moment in the d3d12 exports early enough to get in front of it.
//
// Everything else forwards. Of the twenty exports the real dxgi.dll has, three
// are handled here because the hook has to be installed from one of them, and
// the other seventeen are tail-jump thunks that need no signature. Same shape
// as the D3D12Core* exports in d3d12_thunks.asm and for the same reason.
//
// This DLL is OPTIONAL and separate from d3d12.dll on purpose. Copying it is
// the opt-in: an application that does not need the spoof simply does not get
// this file. It is also useless on its own, since without the d3d12 shim there
// is no Tier 1.1 to unlock.
//
// Build: build_dxgi.bat -> dxgi.dll. Copy it beside d3d12.dll.

#include <windows.h>
#include <dxgi1_6.h>

#include "proxy_log.h"
#include "version.h"
#include "dxgi_spoof.h"

// --- real system dxgi.dll ---------------------------------------------------
//
// By full System32 path, so forwarding never recurses back into this proxy.
static HMODULE RealDXGI() {
    static HMODULE h = [] {
        wchar_t path[MAX_PATH];
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return (HMODULE)nullptr;
        wcscat_s(path, MAX_PATH, L"\\dxgi.dll");
        return LoadLibraryW(path);
    }();
    return h;
}

template <class T>
static T RealProc(const char* name) {
    HMODULE h = RealDXGI();
    return h ? reinterpret_cast<T>(GetProcAddress(h, name)) : nullptr;
}

// --- undocumented exports, forwarded by tail-jump thunk ---------------------
//
// Index order must match the THUNK list at the bottom of dxgi_thunks.asm.
// None of these have public signatures, and a tail jump does not need one.
static const char* const g_thunkNames[] = {
    "ApplyCompatResolutionQuirking",
    "CompatString",
    "CompatValue",
    "DXGID3D10CreateDevice",
    "DXGID3D10CreateLayeredDevice",
    "DXGID3D10GetLayeredDeviceSize",
    "DXGID3D10RegisterLayers",
    "DXGIDeclareAdapterRemovalSupport",
    "DXGIDisableVBlankVirtualization",
    "DXGIDumpJournal",
    "DXGIGetDebugInterface1",
    "DXGIReportAdapterConfiguration",
    "PIXBeginCapture",
    "PIXEndCapture",
    "PIXGetCaptureState",
    "SetAppCompatStringPointer",
    "UpdateHMDEmulationStatus",
};

extern "C" void* DxgiResolveThunk(unsigned index) {
    static void* cache[_countof(g_thunkNames)] = {};
    if (index >= _countof(g_thunkNames)) return nullptr;
    if (!cache[index]) {
        HMODULE h = RealDXGI();
        cache[index] = h ? (void*)GetProcAddress(h, g_thunkNames[index]) : nullptr;
    }
    return cache[index];
}

// --- the three factory entry points -----------------------------------------
//
// Handled here rather than thunked, because the adapter hook has to go in
// before anything can hold an adapter, and an adapter can only come from a
// factory. Installing it here means there is no ordering question left: the
// first factory in the process is the last moment before the first adapter,
// and it is reached long before any engine reads a description.
typedef HRESULT(WINAPI* PFN_Create)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_Create2)(UINT, REFIID, void**);

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    static PFN_Create real = RealProc<PFN_Create>("CreateDXGIFactory");
    if (!real) return E_FAIL;
    spoof::InstallAdapterHook();
    return real(riid, ppFactory);
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    static PFN_Create real = RealProc<PFN_Create>("CreateDXGIFactory1");
    if (!real) return E_FAIL;
    spoof::InstallAdapterHook();
    return real(riid, ppFactory);
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    static PFN_Create2 real = RealProc<PFN_Create2>("CreateDXGIFactory2");
    if (!real) return E_FAIL;
    spoof::InstallAdapterHook();
    return real(Flags, riid, ppFactory);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // No marker of its own. d3d12.dll writes the start and end markers for
        // the run, and a second pair from a second DLL would just make the log
        // harder to read. This says what it is and stops.
        //
        // Nothing is loaded here: the real dxgi is loaded lazily on the first
        // forwarded call, so this stays out of the loader lock.
        ProxyLog("[dxr-tier-11-proxy-log] dxgi proxy attached, shim " DXR_TIER11_VERSION "\n");
    }
    return TRUE;
}
