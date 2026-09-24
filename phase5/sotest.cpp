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
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// Opt in to the Agility SDK, so this probe runs on the SAME D3D12 runtime the
// game does.
//
// Every offline test until now used the OS runtime, 10.0.26100. The game loads
// D3D12Core.dll 1.618.5.0 out of its own Binaries\Win64\D3D12\x64. That is a
// different implementation of CreateStateObject sitting on the same driver,
// and it was never the thing being tested. `sotest --agility` copies nothing
// and assumes phase5out\D3D12\x64\D3D12Core.dll is in place.
//
// These must be exported from the EXE and are read before main runs, which is
// why they are here rather than behind a flag.
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 618; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\x64\\"; }
namespace {

// Must match kPayloadBytes in rq_lower.cpp and rq_pipeline.cpp. A state object
// whose shader config disagrees with the generated shaders is refused with
// E_INVALIDARG, which would look like a result and is not one.
const UINT kPayloadBytes = 92;
const UINT kAttrBytes = 8;

// SOTEST_ADD_RANGES="u0:1001,b4:0": append ONE descriptor table holding these
// ranges, one descriptor each, to the root signature read from disk. TEST
// ONLY. A refused shader is dumped without its root signature, so a newly
// lowered one can only be compile-tested with a borrowed one, and a borrowed
// one can lack a binding or two; the debug layer names exactly which. The
// point is a cold compile on the driver, not a dispatch.
bool AddRanges(ID3D12Device* dev, std::vector<unsigned char>* rs, const char* spec) {
    ID3D12VersionedRootSignatureDeserializer* de = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(rs->data(), rs->size(),
                                                              IID_PPV_ARGS(&de))))
        return false;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* v = nullptr;
    if (FAILED(de->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &v)) || !v) {
        de->Release();
        return false;
    }
    const D3D12_ROOT_SIGNATURE_DESC1& d = v->Desc_1_1;
    std::vector<D3D12_DESCRIPTOR_RANGE1> ranges;
    for (const char* c = spec; *c;) {
        D3D12_DESCRIPTOR_RANGE1 r{};
        r.RangeType = *c == 'u' ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                    : *c == 't' ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                    : *c == 's' ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                                : D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        r.NumDescriptors = 1;
        r.BaseShaderRegister = (UINT)strtoul(c + 1, (char**)&c, 10);
        if (*c == ':') r.RegisterSpace = (UINT)strtoul(c + 1, (char**)&c, 10);
        r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                  D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        if (r.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
            r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        ranges.push_back(r);
        while (*c == ',' || *c == ' ') ++c;
    }
    std::vector<D3D12_ROOT_PARAMETER1> params(d.pParameters, d.pParameters + d.NumParameters);
    D3D12_ROOT_PARAMETER1 t{};
    t.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    t.DescriptorTable.NumDescriptorRanges = (UINT)ranges.size();
    t.DescriptorTable.pDescriptorRanges = ranges.data();
    t.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params.push_back(t);
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC nv{};
    nv.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    nv.Desc_1_1 = d;
    nv.Desc_1_1.NumParameters = (UINT)params.size();
    nv.Desc_1_1.pParameters = params.data();
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = D3D12SerializeVersionedRootSignature(&nv, &blob, &err);
    de->Release();
    if (err) { std::printf("  add ranges: %s\n", (const char*)err->GetBufferPointer()); err->Release(); }
    if (FAILED(hr) || !blob) return false;
    rs->assign((unsigned char*)blob->GetBufferPointer(),
               (unsigned char*)blob->GetBufferPointer() + blob->GetBufferSize());
    blob->Release();
    (void)dev;
    return true;
}

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
    // How many copies of the hit shaders the library carries, one per baked
    // (geometry, contribution) pair, exported as <name>_<k>. 0 for a library
    // written before 0.37.0 or one that reads no record constant.
    UINT baked = 0;
    // `recordsrv=1` (shim 0.40.0 on): the record is read through a local root
    // SRV at t0, so that is the local root signature to build.
    bool recordSrv = false;
    bool known = false;
};

UINT BakedHitGroups() {
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, "SOTEST_HITGROUPS") != 0 || !v) return 0;
    const int g = atoi(v);
    free(v);
    return g > 0 ? (UINT)g : 0;
}

UINT LocalSpace() {
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, "SOTEST_LOCAL_SPACE") != 0 || !v) return 1;
    const UINT s = (UINT)atoi(v);
    free(v);
    return s;
}

bool EnvFlag(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || !v) return false;
    const bool on = v[0] != 0 && v[0] != '0';
    free(v);
    return on;
}

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
    int a = 0, i = 0, b = 0, r = 0, k = 0;
    const int got = sscanf_s(line,
                             "anyhit=%d intersection=%d both=%d recordconstants=%d baked=%d",
                             &a, &i, &b, &r, &k);
    if (got < 4)
        return s;
    s.anyhit = a != 0;
    s.intersection = i != 0;
    s.both = b != 0;
    s.recordConstants = r != 0;
    s.baked = got == 5 && k > 0 ? static_cast<UINT>(k) : 0;
    s.recordSrv = std::strstr(line, "recordsrv=1") != nullptr;
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
    {
        char spec[256] = {};
        if (!rsBlob.empty() &&
            GetEnvironmentVariableA("SOTEST_ADD_RANGES", spec, sizeof(spec)) && spec[0]) {
            const bool added = AddRanges(dev, &rsBlob, spec);
            if (!quiet) std::printf("  root signature + table {%s}: %s\n", spec,
                                    added ? "added" : "FAILED");
        }
    }

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

    // A BAKED library (shim 0.37.0 on, `baked=N` in the shape) carries N
    // copies of its hit shaders, <name>_<k>, one per (geometry, contribution)
    // pair, and no local root signature: the same state object the shim
    // builds, see BuildStateObject in proxy/rq_pipeline.cpp. SOTEST_HITGROUPS=N
    // forces that for a library whose shape file predates the field, which is
    // how the stubs in phase5/cases/driver-crash/ were measured.
    const UINT baked = shape.baked ? shape.baked : BakedHitGroups();
    std::vector<std::wstring> bn;   // per copy: group, closest, any, isect, group proc, closest proc
    bn.reserve(baked * 6);
    for (UINT i = 0; i < baked; ++i) {
        const std::wstring s = L"_" + std::to_wstring(i);
        bn.push_back(L"HitGroup" + s);
        bn.push_back(L"ClosestHit" + s);
        bn.push_back(L"AnyHit" + s);
        bn.push_back(L"Isect" + s);
        bn.push_back(L"HitGroupProc" + s);
        bn.push_back(L"ClosestHitProc" + s);
    }
    std::vector<D3D12_HIT_GROUP_DESC> bakedDescs;
    for (UINT i = 0; i < baked; ++i) {
        const std::wstring* nm = &bn[i * 6];
        D3D12_HIT_GROUP_DESC g{};
        g.HitGroupExport = nm[0].c_str();
        g.ClosestHitShaderImport = nm[1].c_str();
        if (shape.both) {
            g.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
            g.AnyHitShaderImport = nm[2].c_str();
            bakedDescs.push_back(g);
            D3D12_HIT_GROUP_DESC p{};
            p.HitGroupExport = nm[4].c_str();
            p.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
            p.IntersectionShaderImport = nm[3].c_str();
            p.ClosestHitShaderImport = nm[5].c_str();
            bakedDescs.push_back(p);
        } else if (shape.intersection) {
            g.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
            g.IntersectionShaderImport = nm[3].c_str();
            bakedDescs.push_back(g);
        } else {
            g.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
            if (shape.anyhit) g.AnyHitShaderImport = nm[2].c_str();
            bakedDescs.push_back(g);
        }
    }
    if (!quiet && baked)
        std::printf("  baked: %u cop%s of the hit shaders, no local root signature\n",
                    baked, baked == 1 ? "y" : "ies");

    std::vector<D3D12_STATE_SUBOBJECT> subs(16 + bakedDescs.size());
    UINT n = 0;
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc };
    // The unsuffixed groups only exist in a library that was not baked. The
    // SOTEST_HITGROUPS stubs are the exception: they kept the originals, and
    // building them too is what those measurements did.
    if (!shape.baked) {
        subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg };
        if (shape.both) subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgProc };
    }
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgNullTri };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgNullProc };
    for (auto& g : bakedDescs)
        subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &g };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc };
    subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc };
    if (out.globalRs)
        subs[n++] = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs };

    D3D12_LOCAL_ROOT_SIGNATURE lrs{};
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc{};
    const wchar_t* hitExports[4] = {};
    UINT hitExportCount = 0;
    if (shape.recordConstants && baked == 0) {
        // SOTEST_LOCAL_CBV=1 makes the local root signature carry a root CBV
        // DESCRIPTOR instead of root constants. The library is untouched either
        // way: it still reads a cbuffer at b0 space1. Only the parameter type
        // moves, which is what makes this a one-variable test of the driver
        // crash. See phase5/cases/driver-crash/README.md.
        // SOTEST_LOCAL_SRV=1 makes it a root SRV at t0 of that space, for a
        // library that reads its record through a RAW BUFFER instead of a
        // cbuffer (phase5/cases/driver-crash/make_localsrv.py).
        const bool localCbv = EnvFlag("SOTEST_LOCAL_CBV");
        const bool localSrv = shape.recordSrv || EnvFlag("SOTEST_LOCAL_SRV");
        D3D12_ROOT_PARAMETER rp{};
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        if (localSrv) {
            rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            rp.Descriptor.ShaderRegister = 0;
            rp.Descriptor.RegisterSpace = LocalSpace();
        } else if (localCbv) {
            rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rp.Descriptor.ShaderRegister = 0;
            rp.Descriptor.RegisterSpace = LocalSpace();
        } else {
            rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rp.Constants.ShaderRegister = 0;
            rp.Constants.RegisterSpace = LocalSpace();
            rp.Constants.Num32BitValues = 2;
        }
        if (!quiet)
            std::printf("  local root signature: %s\n",
                        localSrv ? "root SRV descriptor"
                        : localCbv ? "root CBV descriptor" : "root constants");

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
    sod.pSubobjects = subs.data();

    if (!quiet) {
        std::printf("calling CreateStateObject with %u subobjects...\n", n);
        std::fflush(stdout);
    }
    out.hr = dev->CreateStateObject(&sod, IID_PPV_ARGS(&out.so));
    if (!quiet) Report("CreateStateObject", out.hr);
    // With DXR_TIER11_DEBUGLAYER=1 the layer is on; print what it said about
    // this call. It reports through OutputDebugString otherwise, which a
    // console never sees, so its silence here would mean nothing.
    {
        ID3D12InfoQueue* iq = nullptr;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&iq)))) {
            for (UINT64 k = 0; k < iq->GetNumStoredMessages(); ++k) {
                SIZE_T len = 0;
                iq->GetMessage(k, nullptr, &len);
                std::vector<char> buf(len);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (SUCCEEDED(iq->GetMessage(k, msg, &len)) &&
                    msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                    std::printf("  debug layer: %s\n", msg->pDescription);
            }
            iq->ClearStoredMessages();
            iq->Release();
        }
    }

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

// How many lowered_NNN sets a folder may hold. It was 64, which silently
// truncated a 261-library test into a 64-library one that then reported
// "device still alive": a cap that turns a test into a weaker test without
// saying so is the same failure mode as a knob that does not move what it
// names. Raised, and the loop still skips missing indices.
static const int kMaxLibs = 1024;


// Every shader dumped from one run, created in sequence and kept ALIVE.
// Unreal creates compute PSOs on WORKER THREADS: FD3D12PipelineState::CreateAsync
// starts an FAsyncTask<FD3D12PipelineStateWorker>. So this shim's
// CreateStateObject calls are concurrent in the game, and were single-threaded
// in every test here.
//
// That is the one axis the offline probe never varied, and DRED now says the
// GPU was not executing anything when the device died: no breadcrumbs, no page
// fault. A driver internal error with neither is a CPU-side failure, and
// creating a DXR state object is the heaviest CPU-side thing this shim asks the
// driver to do.
int RunThreaded(ID3D12Device5* dev, const std::string& dir, int repeat, int threads) {
    std::printf("creating on %d threads, %d passes each\n\n", threads, repeat);
    std::vector<std::thread> pool;
    std::atomic<int> failed{0};
    std::mutex holdLock;
    std::vector<Built> held;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            for (int pass = 0; pass < repeat; ++pass) {
                for (int i = 0; i < kMaxLibs; ++i) {
                    char stem[64];
                    sprintf_s(stem, "lowered_%03d", i);
                    const std::string lib = dir + "\\" + stem + ".out.dxil";
                    FILE* probe = nullptr;
                    if (fopen_s(&probe, lib.c_str(), "rb") != 0 || !probe) continue;
                    fclose(probe);
                    Built b = BuildOne(dev, lib, dir + "\\" + stem + ".rs.bin", true);
                    if (FAILED(b.hr)) {
                        std::printf("  thread %d %s hr=0x%08X %s\n", t, stem,
                                    static_cast<unsigned>(b.hr), HrName(b.hr));
                        b.Release();
                        ++failed;
                    } else {
                        std::lock_guard<std::mutex> g(holdLock);
                        held.push_back(b);
                    }
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    const HRESULT rem = dev->GetDeviceRemovedReason();
    std::printf("\nheld %zu state objects, %d failed\n", held.size(), failed.load());
    if (rem != S_OK)
        std::printf("  DEVICE REMOVED: 0x%08X %s\n",
                    static_cast<unsigned>(rem), HrName(rem));
    else
        std::printf("  device still alive\n");
    for (auto& b : held) b.Release();
    return (failed.load() || rem != S_OK) ? 1 : 0;
}
int RunDir(ID3D12Device5* dev, const std::string& dir, int repeat) {
    std::vector<Built> held;
    int failed = 0;
  for (int pass = 0; pass < repeat; ++pass)
    for (int i = 0; i < kMaxLibs; ++i) {
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
    int repeat = 1;
    int threads = 1;
    std::string dir, libPath, rsPath;
    for (int i = 1; i < argc; ++i) {
        if (_stricmp(argv[i], "warp") == 0) warp = true;
        else if (_stricmp(argv[i], "hw") == 0) warp = false;
        else if (_stricmp(argv[i], "--dir") == 0 && i + 1 < argc) dir = argv[++i];
        // The game creates ours alongside a couple of hundred of its own.
        // Seven at a time is not the same test.
        else if (_stricmp(argv[i], "--repeat") == 0 && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (_stricmp(argv[i], "--threads") == 0 && i + 1 < argc) threads = atoi(argv[++i]);
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

    {
        char v[8] = {};
        if (GetEnvironmentVariableA("DXR_TIER11_DEBUGLAYER", v, sizeof(v)) && v[0] == '1') {
            ID3D12Debug* dbg = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
                dbg->EnableDebugLayer();
                dbg->Release();
                std::printf("debug layer ON\n");
            }
        }
    }
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
    {
        HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
        wchar_t corePath[MAX_PATH] = L"(not loaded)";
        if (core) GetModuleFileNameW(core, corePath, MAX_PATH);
        std::wprintf(L"D3D12Core: %s\n", corePath);
    }
    std::printf("raytracing tier %u\n\n",
                static_cast<unsigned>(o5.RaytracingTier) / 10);

    int rc;
    if (!dir.empty()) {
        rc = threads > 1 ? RunThreaded(dev, dir, repeat, threads)
                         : RunDir(dev, dir, repeat);
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
