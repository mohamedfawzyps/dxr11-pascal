// Replay a lowered library through CreateStateObject, outside the game.
//
// Three runs of a real Unreal game ended with the GPU dying
// (DXGI_ERROR_DRIVER_INTERNAL_ERROR) while the shim was creating state objects
// from its generated libraries. The dump has all seven of them, but there was
// no way to try one without launching the game again, which takes minutes and
// answers one question.
//
// This takes the pair the shim writes:
//
//     lowered_NNN.out.dxil   the library handed to the driver
//     lowered_NNN.rs.bin     the application's root signature blob
//
// and builds the SAME state object rq_pipeline.cpp builds. If the driver falls
// over, it falls over here, on one shader, in a second, with nothing else
// running.
//
//     sotest <lib.dxil> [rs.bin] [warp|hw]
//
// WARP is worth trying first for a library that kills the hardware driver: if
// WARP accepts it the library is probably valid and the fault is NVIDIA's, and
// if WARP refuses it the message says what is wrong with it.
//
// Exit code is 0 when the state object was created, 1 otherwise. A driver that
// takes the process down with it produces neither, which is itself the answer.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
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

bool ReadFileBytes(const char* path, std::vector<unsigned char>* out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
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
    std::string p = libPath;
    const size_t dot = p.rfind(".out.dxil");
    if (dot == std::string::npos) return s;
    p = p.substr(0, dot) + ".shape.txt";
    std::vector<unsigned char> raw;
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
        case 0x887A0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case 0x887A0001: return "DXGI_ERROR_INVALID_CALL";
        default: return "";
    }
}

void Report(const char* what, HRESULT hr) {
    std::printf("  %-28s hr=0x%08X %s\n", what, static_cast<unsigned>(hr), HrName(hr));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: sotest <lib.dxil> [rs.bin] [warp|hw]\n");
        return 2;
    }
    const char* libPath = argv[1];
    const char* rsPath = nullptr;
    bool warp = false;
    for (int i = 2; i < argc; ++i) {
        if (_stricmp(argv[i], "warp") == 0) warp = true;
        else if (_stricmp(argv[i], "hw") == 0) warp = false;
        else rsPath = argv[i];
    }

    std::vector<unsigned char> lib, rsBlob;
    if (!ReadFileBytes(libPath, &lib) || lib.empty()) {
        std::printf("cannot read %s\n", libPath);
        return 2;
    }
    if (rsPath && !ReadFileBytes(rsPath, &rsBlob)) {
        std::printf("cannot read %s\n", rsPath);
        return 2;
    }
    std::printf("library %s, %zu bytes%s\n", libPath, lib.size(),
                rsBlob.empty() ? ", NO root signature blob" : "");

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
    std::printf("raytracing tier %u\n",
                static_cast<unsigned>(o5.RaytracingTier) / 10);

    ID3D12RootSignature* globalRs = nullptr;
    if (!rsBlob.empty()) {
        hr = dev->CreateRootSignature(0, rsBlob.data(), rsBlob.size(),
                                      IID_PPV_ARGS(&globalRs));
        Report("CreateRootSignature", hr);
        if (FAILED(hr)) return 1;
    }

    // --- the same subobjects rq_pipeline.cpp builds -------------------------
    const Shape shape = ReadShape(libPath);
    if (!shape.known) {
        std::printf("no .shape.txt beside the library; it is written by the shim "
                    "from 0.30.0 on and this cannot build the right state object "
                    "without it\n");
        return 2;
    }
    const bool hasAnyHit = shape.anyhit;
    const bool hasIsect = shape.intersection;
    const bool needsBoth = shape.both;
    std::printf("shape: anyhit=%d intersection=%d both=%d recordconstants=%d\n",
                hasAnyHit, hasIsect, needsBoth, shape.recordConstants);

    D3D12_DXIL_LIBRARY_DESC libDesc{};
    libDesc.DXILLibrary.pShaderBytecode = lib.data();
    libDesc.DXILLibrary.BytecodeLength = lib.size();

    D3D12_HIT_GROUP_DESC hg{};
    hg.HitGroupExport = L"HitGroup";
    hg.ClosestHitShaderImport = L"ClosestHit";
    if (needsBoth) {
        hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg.AnyHitShaderImport = L"AnyHit";
    } else if (hasIsect && !hasAnyHit) {
        hg.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        hg.IntersectionShaderImport = L"Isect";
    } else {
        hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        if (hasAnyHit) hg.AnyHitShaderImport = L"AnyHit";
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
    grs.pGlobalRootSignature = globalRs;

    D3D12_STATE_SUBOBJECT subs[11]{};
    UINT n = 0;
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg };
    if (needsBoth) subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgProc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgNullTri };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgNullProc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc };
    if (globalRs)
        subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs };

    // The local root signature carrying the geometry index, added exactly when
    // the shim added it.
    ID3D12RootSignature* localRs = nullptr;
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
                                           IID_PPV_ARGS(&localRs));
        if (blob) blob->Release();
        if (err) err->Release();
        if (SUCCEEDED(rhr) && localRs) {
            lrs.pLocalRootSignature = localRs;
            subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lrs };
            hitExports[hitExportCount++] = L"HitGroup";
            if (needsBoth) hitExports[hitExportCount++] = L"HitGroupProc";
            hitExports[hitExportCount++] = L"HitGroupNullTri";
            hitExports[hitExportCount++] = L"HitGroupNullProc";
            assoc.NumExports = hitExportCount;
            assoc.pExports = hitExports;
            assoc.pSubobjectToAssociate = &subs[n - 1];
            subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
                          &assoc };
        }
    }

    D3D12_STATE_OBJECT_DESC sod{};
    sod.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    sod.NumSubobjects = n;
    sod.pSubobjects = subs;

    std::printf("calling CreateStateObject with %u subobjects...\n", n);
    std::fflush(stdout);

    ID3D12StateObject* so = nullptr;
    hr = dev->CreateStateObject(&sod, IID_PPV_ARGS(&so));
    Report("CreateStateObject", hr);

    if (SUCCEEDED(hr)) {
        // Getting an identifier forces the driver past mere acceptance, which
        // is where a lazy compile would actually happen.
        ID3D12StateObjectProperties* props = nullptr;
        if (SUCCEEDED(so->QueryInterface(IID_PPV_ARGS(&props)))) {
            const void* id = props->GetShaderIdentifier(L"RayGen");
            std::printf("  %-28s %s\n", "GetShaderIdentifier(RayGen)",
                        id ? "returned an identifier" : "returned NULL");
            props->Release();
        }
        so->Release();
    }

    // Ask why, if the device went away. This is the answer the game's crash
    // report gives, reached here in one second instead of a whole launch.
    const HRESULT removed = dev->GetDeviceRemovedReason();
    if (removed != S_OK) Report("GetDeviceRemovedReason", removed);

    if (localRs) localRs->Release();
    if (globalRs) globalRs->Release();
    dev->Release();
    if (adapter) adapter->Release();
    factory->Release();
    return SUCCEEDED(hr) ? 0 : 1;
}
