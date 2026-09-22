// Replay lowered libraries through CreateStateObject, outside the game.
//
// Runs of a real Unreal game kept ending with the GPU dying
// (DXGI_ERROR_DRIVER_INTERNAL_ERROR) while the shim was creating state objects
// from its generated libraries. The dump has them all, but there was no way to
// try one without launching the game again, which takes minutes and answers
// one question.
//
// This takes what the shim writes:
//
//     lowered_NNN.out.dxil     the library handed to the driver
//     lowered_NNN.rs.bin       the root signature it was built against
//     lowered_NNN.shape.txt    the state object shape the shim chose
//
// and builds the SAME state object rq_pipeline.cpp builds.
//
//     sotest <lib.dxil> [rs.bin] [warp|hw]
//     sotest --dir <folder> [warp|hw]
//
// --dir is the one that matters. One at a time proved every library compiles
// on a GTX 1070; the game does not do them one at a time. It creates several,
// HOLDS them all, and Unreal compiles pipelines on a worker pool while the
// renderer submits work. A fault that is cumulative rather than in any single
// library only shows when they are built together.
//
// WARP first is the useful order: if WARP accepts a library the fault is
// likely NVIDIA's, and if WARP refuses it the message says what is wrong.
//
// Exit code is 0 when everything was created. A driver that takes the process
// down with it produces no exit code at all, which is itself the answer.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

// Must match kPayloadBytes in rq_lower.cpp and rq_pipeline.cpp. A state object
// whose shader config disagrees with the generated shaders is refused with
// E_INVALIDARG, which would look like a result and is not one.
const UINT kPayloadBytes = 92;
const UINT kAttrBytes = 8;

bool ReadFileBytes(const std::string& path, std::vector<unsigned char>* out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? static_cast<size_t>(n) : 0);
    const bool ok = n <= 0 || fread(out->data(), 1, out->size(), f) == out->size();
    fclose(f);
    return ok;
}

// What the shim decided about this shader, straight from its own dump.
//
// The first version of this searched the container for export names, and it
// reported an intersection shader in a library that has none. The shim knows
// the answer and now writes it down; inferring it back out of the bytes was
// solving a problem that did not need to exist.
struct Shape {
    bool anyhit = false;
    bool intersection = false;
    bool both = false;
    bool recordConstants = false;
    bool known = false;
};

Shape ReadShape(const std::string& libPath) {
    Shape s;
    const size_t dot = libPath.rfind(".out.dxil");
    if (dot == std::string::npos) return s;
    const std::string p = libPath.substr(0, dot) + ".shape.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) return s;
    char line[256] = {};
    const size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    if (n == 0) return s;
    int a = 0, i = 0, b = 0, r = 0;
    if (sscanf_s(line, "anyhit=%d intersection=%d both=%d recordconstants=%d",
                 &a, &i, &b, &r) != 4)
        return s;
    s.anyhit = a != 0;
    s.intersection = i != 0;
    s.both = b != 0;
    s.recordConstants = r != 0;
    s.known = true;
    return s;
}

const char* HrName(HRESULT hr) {
    switch (static_cast<unsigned>(hr)) {
        case 0x80070057: return "E_INVALIDARG";
        case 0x8007000E: return "E_OUTOFMEMORY";
        case 0x80004005: return "E_FAIL";
        case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED";
        case 0x887A0006: return "DXGI_ERROR_DEVICE_HUNG";
        case 0x887A0007: return "DXGI_ERROR_DEVICE_RESET";
        case 0x887A0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case 0x887A0001: return "DXGI_ERROR_INVALID_CALL";
        default: return "";
    }
}

void Report(const char* what, HRESULT hr) {
    std::printf("  %-28s hr=0x%08X %s\n", what, static_cast<unsigned>(hr), HrName(hr));
}

struct Built {
    ID3D12StateObject* so = nullptr;
    ID3D12RootSignature* globalRs = nullptr;
    ID3D12RootSignature* localRs = nullptr;
    HRESULT hr = E_FAIL;

    void Release() {
        if (so) so->Release();
        if (localRs) localRs->Release();
        if (globalRs) globalRs->Release();
        so = nullptr; localRs = nullptr; globalRs = nullptr;
    }
};

// The same subobjects rq_pipeline.cpp assembles, from the dumped files.
Built BuildOne(ID3D12Device5* dev, const std::string& libPath,
               const std::string& rsPath, bool quiet) {
    Built out;

    std::vector<unsigned char> lib, rsBlob;
    if (!ReadFileBytes(libPath, &lib) || lib.empty()) {
        if (!quiet) std::printf("cannot read %s\n", libPath.c_str());
        return out;
    }
    ReadFileBytes(rsPath, &rsBlob);   // optional, but almost always needed

    const Shape shape = ReadShape(libPath);
    if (!shape.known) {
        if (!quiet)
            std::printf("no .shape.txt beside the library; it is written by the shim "
                        "from 0.30.0 on and this cannot build the right state object "
                        "without it\n");
        return out;
    }
    if (!quiet) {
        std::printf("library %s, %zu bytes%s\n", libPath.c_str(), lib.size(),
                    rsBlob.empty() ? ", NO root signature blob" : "");
        std::printf("shape: anyhit=%d intersection=%d both=%d recordconstants=%d\n",
                    shape.anyhit, shape.intersection, shape.both,
                    shape.recordConstants);
    }

    if (!rsBlob.empty()) {
        out.hr = dev->CreateRootSignature(0, rsBlob.data(), rsBlob.size(),
                                          IID_PPV_ARGS(&out.globalRs));
        if (!quiet) Report("CreateRootSignature", out.hr);
        if (FAILED(out.hr)) return out;
    }

    D3D12_DXIL_LIBRARY_DESC libDesc{};
    libDesc.DXILLibrary.pShaderBytecode = lib.data();
    libDesc.DXILLibrary.BytecodeLength = lib.size();

    D3D12_HIT_GROUP_DESC hg{};
    hg.HitGroupExport = L"HitGroup";
    hg.ClosestHitShaderImport = L"ClosestHit";
    if (shape.both) {
        hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg.AnyHitShaderImport = L"AnyHit";
    } else if (shape.intersection) {
        hg.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        hg.IntersectionShaderImport = L"Isect";
    } else {
        hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        if (shape.anyhit) hg.AnyHitShaderImport = L"AnyHit";
    }

    D3D12_HIT_GROUP_DESC hgProc{};
    hgProc.HitGroupExport = L"HitGroupProc";
    hgProc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
    hgProc.IntersectionShaderImport = L"Isect";
    hgProc.ClosestHitShaderImport = L"ClosestHitProc";

    D3D12_HIT_GROUP_DESC hgNullTri{};
    hgNullTri.HitGroupExport = L"HitGroupNullTri";
    hgNullTri.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hgNullTri.AnyHitShaderImport = L"AnyHitNull";

    D3D12_HIT_GROUP_DESC hgNullProc{};
    hgNullProc.HitGroupExport = L"HitGroupNullProc";
    hgNullProc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
    hgNullProc.IntersectionShaderImport = L"IsectNull";

    D3D12_RAYTRACING_SHADER_CONFIG sc{};
    sc.MaxPayloadSizeInBytes = kPayloadBytes;
    sc.MaxAttributeSizeInBytes = kAttrBytes;

    D3D12_RAYTRACING_PIPELINE_CONFIG pc{};
    pc.MaxTraceRecursionDepth = 1;

    D3D12_GLOBAL_ROOT_SIGNATURE grs{};
    grs.pGlobalRootSignature = out.globalRs;

    D3D12_STATE_SUBOBJECT subs[11]{};
    UINT n = 0;
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg };
    if (shape.both) subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgProc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgNullTri };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgNullProc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc };
    if (out.globalRs)
        subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs };

    D3D12_LOCAL_ROOT_SIGNATURE lrs{};
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc{};
    const wchar_t* hitExports[4] = {};
    UINT hitExportCount = 0;
    if (shape.recordConstants) {
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp.Constants.ShaderRegister = 0;
        rp.Constants.RegisterSpace = 1;
        rp.Constants.Num32BitValues = 2;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters = &rp;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT rhr = D3D12SerializeRootSignature(
            &rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
        if (SUCCEEDED(rhr))
            rhr = dev->CreateRootSignature(0, blob->GetBufferPointer(),
                                           blob->GetBufferSize(),
                                           IID_PPV_ARGS(&out.localRs));
        if (blob) blob->Release();
        if (err) err->Release();
        if (FAILED(rhr) || !out.localRs) {
            if (!quiet) Report("local CreateRootSignature", rhr);
            out.hr = rhr;
            return out;
        }
        lrs.pLocalRootSignature = out.localRs;
        subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lrs };
        hitExports[hitExportCount++] = L"HitGroup";
        if (shape.both) hitExports[hitExportCount++] = L"HitGroupProc";
        hitExports[hitExportCount++] = L"HitGroupNullTri";
        hitExports[hitExportCount++] = L"HitGroupNullProc";
        assoc.NumExports = hitExportCount;
        assoc.pExports = hitExports;
        assoc.pSubobjectToAssociate = &subs[n - 1];
        subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
                      &assoc };
    }

    D3D12_STATE_OBJECT_DESC sod{};
    sod.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    sod.NumSubobjects = n;
    sod.pSubobjects = subs;

    if (!quiet) {
        std::printf("calling CreateStateObject with %u subobjects...\n", n);
        std::fflush(stdout);
    }
    out.hr = dev->CreateStateObject(&sod, IID_PPV_ARGS(&out.so));
    if (!quiet) Report("CreateStateObject", out.hr);

    if (SUCCEEDED(out.hr) && out.so) {
        // Getting an identifier forces the driver past mere acceptance, which
        // is where a lazy compile would actually happen.
        ID3D12StateObjectProperties* props = nullptr;
        if (SUCCEEDED(out.so->QueryInterface(IID_PPV_ARGS(&props)))) {
            const void* id = props->GetShaderIdentifier(L"RayGen");
            if (!quiet)
                std::printf("  %-28s %s\n", "GetShaderIdentifier(RayGen)",
                            id ? "returned an identifier" : "returned NULL");
            props->Release();
        }
    }
    return out;
}

// Every shader dumped from one run, created in sequence and kept ALIVE.
int RunDir(ID3D12Device5* dev, const std::string& dir) {
    std::vector<Built> held;
    int failed = 0;
    for (int i = 0; i < 64; ++i) {
        char stem[64];
        sprintf_s(stem, "lowered_%03d", i);
        const std::string lib = dir + "\\" + stem + ".out.dxil";
        FILE* probe = nullptr;
        if (fopen_s(&probe, lib.c_str(), "rb") != 0 || !probe) continue;
        fclose(probe);

        std::printf("%s ... ", stem);
        std::fflush(stdout);
        Built b = BuildOne(dev, lib, dir + "\\" + stem + ".rs.bin", true);
        if (SUCCEEDED(b.hr)) {
            std::printf("ok\n");
            held.push_back(b);
        } else {
            std::printf("hr=0x%08X %s\n", static_cast<unsigned>(b.hr), HrName(b.hr));
            b.Release();
            ++failed;
        }
        std::fflush(stdout);

        const HRESULT rem = dev->GetDeviceRemovedReason();
        if (rem != S_OK) {
            std::printf("\n  DEVICE REMOVED after %s: 0x%08X %s\n", stem,
                        static_cast<unsigned>(rem), HrName(rem));
            return 1;
        }
    }
    std::printf("\nheld %zu state objects at once, %d failed, device still alive\n",
                held.size(), failed);
    for (auto& b : held) b.Release();
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: sotest <lib.dxil> [rs.bin] [warp|hw]\n"
                    "       sotest --dir <folder> [warp|hw]\n");
        return 2;
    }

    bool warp = false;
    std::string dir, libPath, rsPath;
    for (int i = 1; i < argc; ++i) {
        if (_stricmp(argv[i], "warp") == 0) warp = true;
        else if (_stricmp(argv[i], "hw") == 0) warp = false;
        else if (_stricmp(argv[i], "--dir") == 0 && i + 1 < argc) dir = argv[++i];
        else if (libPath.empty()) libPath = argv[i];
        else rsPath = argv[i];
    }

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        std::printf("CreateDXGIFactory2 failed\n");
        return 2;
    }
    IDXGIAdapter1* adapter = nullptr;
    if (warp) factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    else factory->EnumAdapters1(0, &adapter);

    ID3D12Device5* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr) || !dev) {
        Report("D3D12CreateDevice", hr);
        return 2;
    }
    {
        DXGI_ADAPTER_DESC1 ad{};
        if (adapter) adapter->GetDesc1(&ad);
        std::wprintf(L"adapter %s\n", ad.Description);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    std::printf("raytracing tier %u\n\n",
                static_cast<unsigned>(o5.RaytracingTier) / 10);

    int rc;
    if (!dir.empty()) {
        rc = RunDir(dev, dir);
    } else {
        Built b = BuildOne(dev, libPath, rsPath, false);
        const HRESULT removed = dev->GetDeviceRemovedReason();
        if (removed != S_OK) Report("GetDeviceRemovedReason", removed);
        rc = SUCCEEDED(b.hr) ? 0 : 1;
        b.Release();
    }

    dev->Release();
    if (adapter) adapter->Release();
    factory->Release();
    return rc;
}
