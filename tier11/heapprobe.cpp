// What the shim needs to know before it reserves descriptors of its own at
// the end of an application's shader-visible heap (0.63.0): how large a
// CBV/SRV/UAV heap the device creates (Unreal asks for 1,000,000, the Tier 2
// maximum), whether descriptor heaps share one vtable (so GetDesc can be
// hooked to report the size the application asked for), and whether calling
// slot 8 with the (this, RetVal*) ABI is GetDesc.
//
//   heapprobe.exe [warp|hw] [-debug]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
using Microsoft::WRL::ComPtr;

static const char* ModuleOf(const void* p, char* buf, DWORD n) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(p), &m) || !m) return "(heap memory, no module)";
    if (!GetModuleFileNameA(m, buf, n)) return "?";
    const char* s = std::strrchr(buf, '\\');
    return s ? s + 1 : buf;
}

int main(int argc, char** argv) {
    const bool warp = argc > 1 && !std::strcmp(argv[1], "warp");
    bool debug = false;
    for (int i = 1; i < argc; ++i) debug = debug || !std::strcmp(argv[i], "-debug");
    if (debug) {
        ComPtr<ID3D12Debug> d;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) d->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory6> f;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    ComPtr<IDXGIAdapter1> a;
    if (warp) f->EnumWarpAdapter(IID_PPV_ARGS(&a));
    else f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a));
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) {
        std::printf("no device\n");
        return 1;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    std::printf("%s%s: resource binding tier %d, CBV/SRV/UAV increment %u\n", warp ? "WARP" : "hw",
                debug ? " (debug layer)" : "", (int)o.ResourceBindingTier,
                dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));

    const UINT sizes[] = { 1000000, 1000000 + 4096, 1000000 + 8192, 1048576, 1100000, 2000000 };
    for (UINT n : sizes) {
        D3D12_DESCRIPTOR_HEAP_DESC d{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, n,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        ComPtr<ID3D12DescriptorHeap> h;
        const HRESULT hr = dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h));
        std::printf("  shader-visible heap of %u: hr=%08lX\n", n, (unsigned long)hr);
    }

    ComPtr<ID3D12DescriptorHeap> h1, h2, h3;
    D3D12_DESCRIPTOR_HEAP_DESC d1{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC d3{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    dev->CreateDescriptorHeap(&d1, IID_PPV_ARGS(&h1));
    d1.NumDescriptors = 128;
    dev->CreateDescriptorHeap(&d1, IID_PPV_ARGS(&h2));
    dev->CreateDescriptorHeap(&d3, IID_PPV_ARGS(&h3));
    void** v1 = *reinterpret_cast<void***>(h1.Get());
    void** v2 = *reinterpret_cast<void***>(h2.Get());
    void** v3 = *reinterpret_cast<void***>(h3.Get());
    char b[MAX_PATH];
    std::printf("  vtables: %p %p %p (shader-visible, shader-visible, CPU-only), %s, in %s\n", (void*)v1,
                (void*)v2, (void*)v3, v1 == v2 && v2 == v3 ? "ONE shared" : "NOT shared",
                ModuleOf(v1, b, MAX_PATH));
    typedef D3D12_DESCRIPTOR_HEAP_DESC*(STDMETHODCALLTYPE * GetDescFn)(ID3D12DescriptorHeap*,
                                                                      D3D12_DESCRIPTOR_HEAP_DESC*);
    D3D12_DESCRIPTOR_HEAP_DESC got{};
    reinterpret_cast<GetDescFn>(v2[8])(h2.Get(), &got);
    const D3D12_DESCRIPTOR_HEAP_DESC want = h2->GetDesc();
    std::printf("  slot 8 called as GetDesc: %u descriptors, flags %u (GetDesc says %u, %u): %s\n",
                got.NumDescriptors, (unsigned)got.Flags, want.NumDescriptors, (unsigned)want.Flags,
                !std::memcmp(&got, &want, sizeof(got)) ? "SAME" : "DIFFERENT");
    std::printf("  device removed reason: %08lX\n", (unsigned long)dev->GetDeviceRemovedReason());
    return 0;
}
