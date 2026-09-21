// Phase 3a - forwarding-only proxy d3d12.dll.
//
// When this DLL is placed next to an application's exe, Windows loads it instead
// of the system d3d12.dll (the app directory is searched before System32). This
// build forwards every export straight to the real system d3d12.dll and does
// nothing else, except log each D3D12CreateDevice call so we can confirm the app
// is running through the proxy. No device wrapping, no translation yet.
//
// The real runtime is loaded by full System32 path, so forwarding never recurses
// back into this proxy.
//
// Build: build_proxy.bat -> d3d12.dll. Copy it next to the target exe.
// Log: %TEMP%\dxr11_proxy.log (and OutputDebugString).

#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdarg>
#include <cwchar>

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

// --- logging ---------------------------------------------------------------
static void Log(const char* fmt, ...) {
    char buf[512];
    va_list a; va_start(a, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    OutputDebugStringA(buf);
    static FILE* f = [] {
        char p[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, p);
        if (n == 0 || n >= MAX_PATH) return (FILE*)nullptr;
        strcat_s(p, MAX_PATH, "dxr11_proxy.log");
        FILE* fp = nullptr; fopen_s(&fp, p, "a");
        return fp;
    }();
    if (f) { std::fputs(buf, f); std::fflush(f); }
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
    Log("[dxr11-proxy] D3D12CreateDevice fl=0x%x hr=0x%08lx device=%p\n",
        (unsigned)fl, (unsigned long)hr, (ppDevice && SUCCEEDED(hr)) ? *ppDevice : nullptr);
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

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) Log("[dxr11-proxy] attached to process\n");
    return TRUE;
}
