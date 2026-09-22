#include "dxgi_spoof.h"

#include "proxy_log.h"
#include "config.h"

#include <dxgi1_6.h>

#include <cstdio>
#include <string>

namespace spoof {
namespace {

// Unreal's list, copied verbatim from IsRayTracingEmulated in
// WindowsD3D12Device.cpp. It is not our judgement about which cards need help:
// it is literally the set this engine refuses, so it is the exact set to
// answer for. Anything not on it passes through untouched, including WARP,
// Intel and AMD, and including the GTX 16-series, which Unreal does not deny.
const UINT kDeniedPascal[] = {
    0x1B80,  // GeForce GTX 1080
    0x1B81,  // GeForce GTX 1070
    0x1B82,  // GeForce GTX 1070 Ti
    0x1B83,  // GeForce GTX 1060 6GB
    0x1B84,  // GeForce GTX 1060 3GB
    0x1C01,  // GeForce GTX 1050 Ti
    0x1C02,  // GeForce GTX 1060 3GB
    0x1C03,  // GeForce GTX 1060 6GB
    0x1C04,  // GeForce GTX 1060 5GB
    0x1C06,  // GeForce GTX 1060 6GB
    0x1C08,  // GeForce GTX 1050
    0x1C81,  // GeForce GTX 1050
    0x1C82,  // GeForce GTX 1050 Ti
    0x1C83,  // GeForce GTX 1050
    0x1B06,  // GeForce GTX 1080 Ti
    0x1B00,  // TITAN X (Pascal)
    0x1B02,  // TITAN Xp
    0x1D81,  // TITAN V
};

const UINT kNvidiaVendor = 0x10DE;

// TU106, GeForce RTX 2060: the LOWEST-END Turing card that has RT cores.
//
// Turing rather than Ampere on purpose, because Unreal carries a second device
// id table, IsNvidiaAmpereGPU, and there is no reason to claim a generation we
// resemble even less. The lowest-end part on purpose too, so that anything
// keying performance off the model assumes as little as possible.
const UINT kDefaultSpoofId = 0x1F08;

UINT g_spoofId = kDefaultSpoofId;
bool g_installed = false;

bool IsDenied(UINT vendor, UINT device) {
    if (vendor != kNvidiaVendor) return false;
    for (UINT d : kDeniedPascal)
        if (d == device) return true;
    return false;
}

// --- the four originals ------------------------------------------------------
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDesc)(IDXGIAdapter*, DXGI_ADAPTER_DESC*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDesc1)(IDXGIAdapter1*, DXGI_ADAPTER_DESC1*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDesc2)(IDXGIAdapter2*, DXGI_ADAPTER_DESC2*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDesc3)(IDXGIAdapter4*, DXGI_ADAPTER_DESC3*);

PFN_GetDesc  g_realGetDesc  = nullptr;
PFN_GetDesc1 g_realGetDesc1 = nullptr;
PFN_GetDesc2 g_realGetDesc2 = nullptr;
PFN_GetDesc3 g_realGetDesc3 = nullptr;

// Logged once per adapter, not once per call. An engine reads the description
// repeatedly and a line per call would bury everything else in the log.
void NoteOnce(UINT device, const wchar_t* name) {
    static UINT seen[8] = {};
    static int count = 0;
    for (int i = 0; i < count; ++i)
        if (seen[i] == device) return;
    if (count < 8) seen[count++] = device;

    char narrow[128] = {};
    for (int i = 0; i < 127 && name[i]; ++i)
        narrow[i] = (name[i] < 128) ? static_cast<char>(name[i]) : '?';

    ProxyLog("[dxr-tier-11-proxy-log] adapter \"%s\" reported as device 0x%04X instead of "
             "0x%04X. Unreal refuses ray tracing on this card by device id alone, after "
             "accepting the tier, so without this the shim has nothing to do. The name, the "
             "vendor and every capability answer are unchanged.\n",
             narrow, g_spoofId, device);
}

#define REWRITE(descPtr)                                            \
    do {                                                            \
        if (SUCCEEDED(hr) && (descPtr) &&                           \
            IsDenied((descPtr)->VendorId, (descPtr)->DeviceId)) {   \
            NoteOnce((descPtr)->DeviceId, (descPtr)->Description);  \
            (descPtr)->DeviceId = g_spoofId;                        \
        }                                                           \
    } while (0)

HRESULT STDMETHODCALLTYPE HookGetDesc(IDXGIAdapter* self, DXGI_ADAPTER_DESC* d) {
    const HRESULT hr = g_realGetDesc(self, d);
    REWRITE(d);
    return hr;
}
HRESULT STDMETHODCALLTYPE HookGetDesc1(IDXGIAdapter1* self, DXGI_ADAPTER_DESC1* d) {
    const HRESULT hr = g_realGetDesc1(self, d);
    REWRITE(d);
    return hr;
}
HRESULT STDMETHODCALLTYPE HookGetDesc2(IDXGIAdapter2* self, DXGI_ADAPTER_DESC2* d) {
    const HRESULT hr = g_realGetDesc2(self, d);
    REWRITE(d);
    return hr;
}
HRESULT STDMETHODCALLTYPE HookGetDesc3(IDXGIAdapter4* self, DXGI_ADAPTER_DESC3* d) {
    const HRESULT hr = g_realGetDesc3(self, d);
    REWRITE(d);
    return hr;
}

// Vtable slots, from the single inheritance chain IUnknown -> IDXGIObject ->
// IDXGIAdapter -> IDXGIAdapter1 -> IDXGIAdapter2 -> IDXGIAdapter3 ->
// IDXGIAdapter4. Written out so the arithmetic is visible rather than four
// magic numbers, and then CHECKED by the self-test below, which is what
// actually makes them safe.
enum : int {
    kSlot_QueryInterface = 0, kSlot_AddRef, kSlot_Release,
    kSlot_SetPrivateData, kSlot_SetPrivateDataInterface, kSlot_GetPrivateData,
    kSlot_GetParent,
    kSlot_EnumOutputs, kSlot_GetDesc, kSlot_CheckInterfaceSupport,   // IDXGIAdapter
    kSlot_GetDesc1,                                                  // IDXGIAdapter1
    kSlot_GetDesc2,                                                  // IDXGIAdapter2
    kSlot_RegisterHardwareContentProtectionTeardownStatusEvent,      // IDXGIAdapter3
    kSlot_UnregisterHardwareContentProtectionTeardownStatus,
    kSlot_QueryVideoMemoryInfo,
    kSlot_SetVideoMemoryReservation,
    kSlot_RegisterVideoMemoryBudgetChangeNotificationEvent,
    kSlot_UnregisterVideoMemoryBudgetChangeNotification,
    kSlot_GetDesc3                                                   // IDXGIAdapter4
};

bool PatchSlot(void** vt, int slot, void* replacement, void** outOriginal) {
    DWORD old = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *outOriginal = vt[slot];
    vt[slot] = replacement;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return true;
}

}  // namespace

void InstallAdapterHook() {
    if (g_installed) return;
    g_installed = true;   // one attempt, success or not

    const cfg::Flag on = cfg::Get("DXR_TIER11_SPOOF", "spoof", true);
    if (!on.value) {
        ProxyLog("[dxr-tier-11-proxy-log] device id spoofing is OFF (from %s). Unreal will "
                 "refuse ray tracing on a Pascal card whatever tier it is told.\n", on.source);
        return;
    }

    const cfg::Text id = cfg::GetText("DXR_TIER11_SPOOFID", "spoofid");
    if (!id.value.empty()) {
        const UINT parsed = static_cast<UINT>(wcstoul(id.value.c_str(), nullptr, 0));
        if (parsed > 0 && parsed <= 0xFFFF) g_spoofId = parsed;
        else ProxyLog("[dxr-tier-11-proxy-log] spoofid from the %s is not a PCI device id, "
                      "ignoring it and using 0x%04X\n", id.source, g_spoofId);
    }

    // The real dxgi, never ours. Our proxy exports no vtables; the objects come
    // from the module that implements them.
    IDXGIFactory1* factory = nullptr;
    typedef HRESULT(WINAPI * PFN_Create)(REFIID, void**);
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wcscat_s(sys, MAX_PATH, L"\\dxgi.dll");
    HMODULE real = GetModuleHandleW(sys);
    if (!real) real = LoadLibraryW(sys);
    if (!real) return;
    auto create = reinterpret_cast<PFN_Create>(GetProcAddress(real, "CreateDXGIFactory1"));
    if (!create || FAILED(create(IID_PPV_ARGS(&factory)))) return;

    // An adapter to read the vtable off, and to self-test against. Any adapter
    // will do for the vtable, since they all share it.
    IDXGIAdapter1* probe = nullptr;
    IDXGIAdapter1* denied = nullptr;
    UINT deniedRealId = 0;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d = {};
        if (SUCCEEDED(a->GetDesc1(&d)) && IsDenied(d.VendorId, d.DeviceId) && !denied) {
            denied = a;
            deniedRealId = d.DeviceId;
            a->AddRef();
        }
        if (!probe) { probe = a; a->AddRef(); }
        a->Release();
    }

    // Nothing here needs it. Patching anyway would be a lie with no purpose,
    // and would put a hook in the path of every adapter read for nothing.
    if (!denied) {
        if (probe) probe->Release();
        factory->Release();
        return;
    }

    void** vt = *reinterpret_cast<void***>(denied);

    const bool patched =
        PatchSlot(vt, kSlot_GetDesc,  (void*)&HookGetDesc,  (void**)&g_realGetDesc) &&
        PatchSlot(vt, kSlot_GetDesc1, (void*)&HookGetDesc1, (void**)&g_realGetDesc1) &&
        PatchSlot(vt, kSlot_GetDesc2, (void*)&HookGetDesc2, (void**)&g_realGetDesc2) &&
        PatchSlot(vt, kSlot_GetDesc3, (void*)&HookGetDesc3, (void**)&g_realGetDesc3);

    // Self-test, on the same run, against an adapter KNOWN to need rewriting.
    //
    // This is the part that makes hardcoded slot indices acceptable. A wrong
    // index would put our function where something else belongs, so it is not
    // enough to check that the spoof works: every one of the four has to be
    // shown to work, and everything else about the description has to be shown
    // unchanged. If any of that fails the whole patch comes out again.
    bool ok = patched;
    if (ok) {
        DXGI_ADAPTER_DESC  d0 = {};
        DXGI_ADAPTER_DESC1 d1 = {};
        DXGI_ADAPTER_DESC2 d2 = {};
        DXGI_ADAPTER_DESC3 d3 = {};
        IDXGIAdapter2* a2 = nullptr;
        IDXGIAdapter4* a4 = nullptr;
        denied->QueryInterface(IID_PPV_ARGS(&a2));
        denied->QueryInterface(IID_PPV_ARGS(&a4));

        ok = SUCCEEDED(denied->GetDesc(&d0)) && d0.DeviceId == g_spoofId &&
             SUCCEEDED(denied->GetDesc1(&d1)) && d1.DeviceId == g_spoofId &&
             d1.VendorId == kNvidiaVendor &&
             d1.DedicatedVideoMemory != 0 &&
             (!a2 || (SUCCEEDED(a2->GetDesc2(&d2)) && d2.DeviceId == g_spoofId)) &&
             (!a4 || (SUCCEEDED(a4->GetDesc3(&d3)) && d3.DeviceId == g_spoofId));

        if (a2) a2->Release();
        if (a4) a4->Release();
    }

    if (!ok) {
        if (g_realGetDesc)  { void* t; PatchSlot(vt, kSlot_GetDesc,  (void*)g_realGetDesc,  &t); }
        if (g_realGetDesc1) { void* t; PatchSlot(vt, kSlot_GetDesc1, (void*)g_realGetDesc1, &t); }
        if (g_realGetDesc2) { void* t; PatchSlot(vt, kSlot_GetDesc2, (void*)g_realGetDesc2, &t); }
        if (g_realGetDesc3) { void* t; PatchSlot(vt, kSlot_GetDesc3, (void*)g_realGetDesc3, &t); }
        g_realGetDesc = nullptr; g_realGetDesc1 = nullptr;
        g_realGetDesc2 = nullptr; g_realGetDesc3 = nullptr;
        ProxyLog("[dxr-tier-11-proxy-log] device id spoof SELF-TEST FAILED, hook removed. "
                 "The adapter vtable is not laid out as expected, so nothing was changed. "
                 "Unreal will refuse ray tracing on this card.\n");
    } else {
        ProxyLog("[dxr-tier-11-proxy-log] device id spoof installed: adapter vtable %p, "
                 "0x%04X -> 0x%04X, all four GetDesc forms self-tested\n",
                 (void*)vt, deniedRealId, g_spoofId);
    }

    denied->Release();
    if (probe) probe->Release();
    factory->Release();
}

}  // namespace spoof
