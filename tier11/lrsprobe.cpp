// How large a LOCAL root signature the device's driver takes (0.59.0). On a
// GTX 1070 one past the limit REMOVES THE DEVICE inside CreateRootSignature
// (DXGI_ERROR_DRIVER_INTERNAL_ERROR); WARP took 520 root descriptors. So
// every run on the hardware ends with the device removed: it is a
// measurement, not a test to run in a suite. The shim's rule, which covers
// every point below, is in proxy/geom_index_so.cpp (Extend).
//
//   lrsprobe.exe [warp|hw] [mode] [constants]
//   mode 0  root SRVs only                         1070: 96 work, 97 remove it
//   mode 1  one constants parameter of n dwords    128 work
//   mode 2  <constants> dwords, then root SRVs     1:95 16:88 32:80 64:64
//                                                  65:31 80:24 100:14 127:0
//   mode 3  the same over two parameters           32:80 64:64 80:44 100:39
//   mode 4  descriptor tables only                 192 work
//   mode 5  32 constants, 40 root SRVs, then tables  80 work
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {
    bool warp = argc > 1 && !strcmp(argv[1], "warp");
    // mode: 0 root SRVs; 1 one root-constant parameter of n dwords; 2 128
    // dwords of constants first, then n root SRVs.
    const int mode = argc > 2 ? atoi(argv[2]) : 0;
    ComPtr<IDXGIFactory6> f; CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    ComPtr<IDXGIAdapter1> a;
    if (warp) f->EnumWarpAdapter(IID_PPV_ARGS(&a));
    else f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a));
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) return 1;
    int lastOk = -1;
    for (UINT n = 1; n <= 520; ++n) {
        std::vector<D3D12_ROOT_PARAMETER1> p;
        if (mode == 1) {
            p.resize(1);
            p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            p[0].Constants.Num32BitValues = n;
        } else {
            if (mode == 3) {   // the constants split over two parameters
                const UINT c = argc > 3 ? (UINT)atoi(argv[3]) : 64;
                for (UINT h = 0; h < 2; ++h) {
                    D3D12_ROOT_PARAMETER1 q{};
                    q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                    q.Constants.Num32BitValues = h ? c - c / 2 : c / 2;
                    q.Constants.ShaderRegister = 5 + h;
                    p.push_back(q);
                }
            }
            if (mode == 5) {   // 32 constants and 40 root SRVs first, then n tables
                D3D12_ROOT_PARAMETER1 q{};
                q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                q.Constants.Num32BitValues = 32;
                q.Constants.ShaderRegister = 5;
                p.push_back(q);
                for (UINT i = 0; i < 40; ++i) {
                    D3D12_ROOT_PARAMETER1 r{};
                    r.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                    r.Descriptor.ShaderRegister = 300 + i;
                    r.Descriptor.RegisterSpace = 0x7FFF0000;
                    p.push_back(r);
                }
            }
            if (mode == 4 || mode == 5) {   // n descriptor tables instead of root SRVs
                static D3D12_DESCRIPTOR_RANGE1 rg[600];
                for (UINT i = 0; i < n; ++i) {
                    rg[i] = D3D12_DESCRIPTOR_RANGE1{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i, 0x7FFF0000,
                                                     D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0 };
                    D3D12_ROOT_PARAMETER1 q{};
                    q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                    q.DescriptorTable = { 1, &rg[i] };
                    p.push_back(q);
                }
            }
            if (mode == 2) {
                p.emplace_back();
                p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                p[0].Constants.Num32BitValues = argc > 3 ? (UINT)atoi(argv[3]) : 128;
                p[0].Constants.ShaderRegister = 5;
            }
            for (UINT i = 0; i < n && mode != 4 && mode != 5; ++i) {
                D3D12_ROOT_PARAMETER1 q{};
                q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
                q.Descriptor.ShaderRegister = i;
                q.Descriptor.RegisterSpace = 0x7FFF0000;
                p.push_back(q);
            }
        }
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{};
        d.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        d.Desc_1_1 = { (UINT)p.size(), p.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE };
        ComPtr<ID3DBlob> blob, err;
        HRESULT hs = D3D12SerializeVersionedRootSignature(&d, &blob, &err);
        ComPtr<ID3D12RootSignature> rs;
        HRESULT hc = SUCCEEDED(hs) ? dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                               IID_PPV_ARGS(&rs)) : hs;
        if (SUCCEEDED(hc)) { lastOk = (int)n; continue; }
        std::printf("%s mode %d: n=%u: serialize hr=%08lX create hr=%08lX %s\n", warp ? "WARP" : "hw", mode, n,
                    (unsigned long)hs, (unsigned long)hc,
                    err ? (const char*)err->GetBufferPointer() : "");
        break;
    }
    std::printf("largest that worked: %d; device removed reason after: %08lX\n", lastOk,
                (unsigned long)dev->GetDeviceRemovedReason());
    return 0;
}
