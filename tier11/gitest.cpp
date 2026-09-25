// GeometryIndex() in an APPLICATION's own DXR 1.0 hit shaders, against WARP.
//
// A small stand-in for a game: its own state object, its own local root
// signatures, its own shader table, its own TraceRay arguments. Its hit
// shaders read GeometryIndex(), which is Tier 1.1: the GTX 1070's driver
// rejects the whole library. The shim has to make it work without the
// application knowing. WARP runs Tier 1.1 natively and is the ground truth.
//
// Every layout is a different way a real application arranges its shader
// table, because the whole difficulty lives there: a DXR 1.0 hit shader can
// only tell which geometry it serves from WHICH RECORD it runs from.
//
//   mult    one record per geometry (multiplier 1). Unreal's arrangement.
//   slots   two ray types interleaved (multiplier 2), the odd records a
//           different hit group that never reads GeometryIndex.
//   zero1   multiplier 0: every geometry of the one instance on ONE record.
//   zero    multiplier 0, two instances on neighbouring records, so no
//           multiplier the shim could pick separates them in place.
//   shared  two instances of one BLAS on the same records, and a third.
//   anyhit  an any-hit shader that reads GeometryIndex() too, and ignores
//           geometry 1, so a wrong answer changes which surface is hit.
//   mixrs   two hit groups with different local root signature sizes, so
//           the record layout differs from record to record.
//
// Each pixel records (GeometryIndex, InstanceIndex, PrimitiveIndex, the
// record's own local root constant). The last one proves the application's
// own record data still arrives intact, whatever the shim does to the table.
//
// Build:  build_tier11.bat          -> gitest.exe in the repository root,
//                                      beside the proxy d3d12.dll and DXC
// Run:    gitest.exe [--sm66] [--collections | --grow] [--table] [layout ...]
//         default: every layout, one state object
//
// --table binds the scene through a DESCRIPTOR TABLE instead of a root SRV,
// with a second scene live whose layout conflicts with the real one, and
// that decoy's descriptor on both sides of the real one in the heap. The
// real descriptor arrives by CopyDescriptors from a staging heap, over a
// decoy written there first. The shim has to find WHICH scene is traced.
//
// Exit code 0 only when every layout matches WARP.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <stdexcept>
#include <algorithm>

using Microsoft::WRL::ComPtr;

#ifndef DXRTEST_AGILITY_VERSION
  #define DXRTEST_AGILITY_VERSION 619u
#endif
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = DXRTEST_AGILITY_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

struct HrError : std::runtime_error {
    HRESULT hr;
    HrError(const std::string& s, HRESULT h) : std::runtime_error(s), hr(h) {}
};
static void HR(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char b[256];
        std::snprintf(b, sizeof(b), "%s failed, hr=0x%08X", what, (unsigned)hr);
        throw HrError(b, hr);
    }
}

static bool g_sm66 = false;
// 0 one state object, 1 --collections (Unreal's way), 2 --grow (the hit
// collection linked by AddToStateObject, how Unreal grows its pipeline).
static int g_mode = 0;
static bool g_table = false;

// --- the application's shaders ------------------------------------------------
// M_VAL, R_VAL: the TraceRay multiplier and ray contribution, literal as in a
// real engine. ANYHIT: the any-hit variant. Two closest-hits, because mixrs
// needs a second local root signature layout.
static const char* kLib = R"HLSL(
cbuffer CB : register(b0) { uint W; uint H; uint NInst; uint Pad; };
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<uint4> Out : register(u0);

cbuffer Rec : register(b0, space1) { uint RecTag; };
cbuffer Rec3 : register(b1, space1) { uint RecA; uint RecB; uint RecC; };

struct Pay { uint4 v; };

[shader("raygeneration")] void RayGen() {
    uint2 p = DispatchRaysIndex().xy;
    RayDesc r;
    r.Origin = float3((p.x + 0.5) / W * NInst, (p.y + 0.5) / H, 1.0);
    r.Direction = float3(0, 0, -1);
    r.TMin = 0.0; r.TMax = 10.0;
    Pay pay; pay.v = uint4(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
#if ANYHIT
    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, R_VAL, M_VAL, 0, r, pay);
#else
    TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, R_VAL, M_VAL, 0, r, pay);
#endif
    Out[p.y * W + p.x] = pay.v;
}

[shader("miss")] void Miss(inout Pay p) { p.v = uint4(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF); }

[shader("closesthit")]
void ClosestHit(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    p.v = uint4(GeometryIndex(), InstanceIndex(), PrimitiveIndex(), RecTag);
}
[shader("closesthit")]
void ClosestHit3(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    p.v = uint4(GeometryIndex() + 100, InstanceIndex(), PrimitiveIndex(), RecA + RecB * 7 + RecC * 31);
}
// Never reads GeometryIndex: the shim must leave it, and its records, alone.
[shader("closesthit")]
void ClosestHitOther(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    p.v = uint4(777, InstanceIndex(), PrimitiveIndex(), RecTag);
}
[shader("anyhit")]
void AnyHit(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    if (GeometryIndex() == 1) IgnoreHit();
}
)HLSL";

static ComPtr<IDxcBlob> Compile(int R, int M, bool anyhit) {
    static HMODULE m = LoadLibraryW(L"dxcompiler.dll");
    if (!m) throw std::runtime_error("dxcompiler.dll not found");
    auto create = (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance");
    ComPtr<IDxcCompiler3> c;
    HR(create(CLSID_DxcCompiler, IID_PPV_ARGS(&c)), "create IDxcCompiler3");
    std::wstring dr = L"R_VAL=" + std::to_wstring(R), dm = L"M_VAL=" + std::to_wstring(M);
    std::wstring da = std::wstring(L"ANYHIT=") + (anyhit ? L"1" : L"0");
    std::vector<const wchar_t*> args = { L"-T", g_sm66 ? L"lib_6_6" : L"lib_6_5",
                                         L"-D", dr.c_str(), L"-D", dm.c_str(), L"-D", da.c_str() };
    DxcBuffer buf{ kLib, std::strlen(kLib), DXC_CP_UTF8 };
    ComPtr<IDxcResult> res;
    HR(c->Compile(&buf, args.data(), (UINT32)args.size(), nullptr, IID_PPV_ARGS(&res)), "Compile");
    HRESULT st = E_FAIL; res->GetStatus(&st);
    if (FAILED(st)) {
        ComPtr<IDxcBlobUtf8> e;
        res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&e), nullptr);
        throw std::runtime_error(std::string("DXC: ") + (e ? e->GetStringPointer() : "?"));
    }
    ComPtr<IDxcBlob> obj;
    HR(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr), "DXC object");
    return obj;
}

// --- layouts --------------------------------------------------------------------
enum Blas { kA, kB };   // A: 4 geometries (quadrants), B: 2 geometries (halves)
struct Inst { Blas blas; UINT contribution; };
struct Layout {
    const char* name;
    std::vector<Inst> inst;
    int R, M;
    UINT records;
    bool anyhit;
    // Per record, which hit group: 0 ClosestHit, 1 ClosestHit3, 2 ClosestHitOther.
    std::vector<int> group;
};
static std::vector<Layout> Layouts() {
    std::vector<Layout> v;
    v.push_back({ "mult",   { {kA, 0}, {kB, 4} },           0, 1, 6,  false, {} });
    v.push_back({ "slots",  { {kA, 0}, {kB, 8} },           0, 2, 12, false, {} });
    v.push_back({ "zero1",  { {kA, 0} },                    0, 0, 1,  false, {} });
    v.push_back({ "zero",   { {kA, 0}, {kB, 1} },           0, 0, 2,  false, {} });
    v.push_back({ "shared", { {kA, 0}, {kA, 0}, {kB, 4} },  0, 1, 6,  false, {} });
    v.push_back({ "anyhit", { {kA, 0}, {kB, 4} },           0, 1, 6,  true,  {} });
    v.push_back({ "mixrs",  { {kA, 0}, {kB, 4} },           0, 1, 6,  false, {} });
    for (auto& l : v) {
        l.group.assign(l.records, 0);
        if (!std::strcmp(l.name, "slots"))
            for (UINT i = 1; i < l.records; i += 2) l.group[i] = 2;
        if (!std::strcmp(l.name, "mixrs"))
            for (UINT i = 1; i < l.records; i += 2) l.group[i] = 1;
    }
    return v;
}

// --- D3D12 helpers ------------------------------------------------------------
static ComPtr<ID3D12Resource> Buffer(ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE heap,
        D3D12_RESOURCE_STATES state, bool uav = false) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
        IID_PPV_ARGS(&r)), "CreateCommittedResource");
    return r;
}
static void UavAll(ID3D12GraphicsCommandList* cl) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl->ResourceBarrier(1, &b);
}

struct Gpu {
    ComPtr<ID3D12Device5> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList4> cl;
    ComPtr<ID3D12Fence> f;
    UINT64 fv = 0;
    HANDLE ev = nullptr;
    void init(IDXGIAdapter1* ad) {
        HR(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)), "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)), "queue");
        HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)), "alloc");
        HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr,
            IID_PPV_ARGS(&cl)), "list");
        HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)), "fence");
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    void flush() {
        HR(cl->Close(), "Close");
        ID3D12CommandList* l[] = { cl.Get() };
        q->ExecuteCommandLists(1, l);
        HR(q->Signal(f.Get(), ++fv), "Signal");
        if (f->GetCompletedValue() < fv) {
            HR(f->SetEventOnCompletion(fv, ev), "SetEventOnCompletion");
            WaitForSingleObject(ev, INFINITE);
        }
        HR(dev->GetDeviceRemovedReason(), "device (removed)");
        HR(al->Reset(), "allocator Reset");
        HR(cl->Reset(al.Get(), nullptr), "list Reset");
    }
};

static ComPtr<ID3D12Resource> BuildAS(Gpu& g, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in,
                                      std::vector<ComPtr<ID3D12Resource>>& keep) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    g.dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    if (!info.ResultDataMaxSizeInBytes) throw std::runtime_error("prebuild info zero");
    auto scratch = Buffer(g.dev.Get(), info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    auto as = Buffer(g.dev.Get(), info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.Inputs = in;
    d.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    d.DestAccelerationStructureData = as->GetGPUVirtualAddress();
    g.cl->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
    UavAll(g.cl.Get());
    keep.push_back(scratch);
    return as;
}

// A unit square split into geometries: A into 4 quadrants, B into 2 halves.
// Each geometry is two triangles, so PrimitiveIndex is 0 or 1 in each.
static ComPtr<ID3D12Resource> BuildBlas(Gpu& g, Blas which, bool opaque,
                                        std::vector<ComPtr<ID3D12Resource>>& keep) {
    std::vector<float> v;
    auto quad = [&](float x0, float y0, float x1, float y1) {
        const float t[18] = { x0, y0, 0, x1, y0, 0, x1, y1, 0,  x0, y0, 0, x1, y1, 0, x0, y1, 0 };
        v.insert(v.end(), t, t + 18);
    };
    if (which == kA) {
        quad(0, 0, .5f, .5f); quad(.5f, 0, 1, .5f); quad(0, .5f, .5f, 1); quad(.5f, .5f, 1, 1);
    } else {
        quad(0, 0, .5f, 1); quad(.5f, 0, 1, 1);
    }
    const UINT n = (UINT)(v.size() / 18);
    auto vb = Buffer(g.dev.Get(), v.size() * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* m = nullptr; HR(vb->Map(0, nullptr, &m), "Map vb");
    std::memcpy(m, v.data(), v.size() * 4); vb->Unmap(0, nullptr);
    keep.push_back(vb);
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geo(n);
    for (UINT k = 0; k < n; ++k) {
        auto& d = geo[k];
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        d.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress() + (UINT64)k * 72;
        d.Triangles.VertexBuffer.StrideInBytes = 12;
        d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        d.Triangles.VertexCount = 6;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.NumDescs = n; in.pGeometryDescs = geo.data();
    return BuildAS(g, in, keep);
}

static ComPtr<ID3D12RootSignature> RootSig(ID3D12Device* dev, const D3D12_ROOT_SIGNATURE_DESC& d) {
    ComPtr<ID3DBlob> b, e;
    HR(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e), "serialize RS");
    ComPtr<ID3D12RootSignature> rs;
    HR(dev->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&rs)),
       "CreateRootSignature");
    return rs;
}

static const UINT kH = 128, kWPer = 128;

// Runs one layout on one device and returns the image.
static std::vector<uint32_t> Run(IDXGIAdapter1* ad, const Layout& L) {
    Gpu g; g.init(ad);
    std::vector<ComPtr<ID3D12Resource>> keep;
    auto blasA = BuildBlas(g, kA, !L.anyhit, keep);
    auto blasB = BuildBlas(g, kB, !L.anyhit, keep);
    const UINT n = (UINT)L.inst.size();
    auto ib = Buffer(g.dev.Get(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * n, D3D12_HEAP_TYPE_UPLOAD,
                     D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RAYTRACING_INSTANCE_DESC* id = nullptr;
    HR(ib->Map(0, nullptr, (void**)&id), "Map instances");
    for (UINT i = 0; i < n; ++i) {
        std::memset(&id[i], 0, sizeof(id[i]));
        id[i].Transform[0][0] = id[i].Transform[1][1] = id[i].Transform[2][2] = 1.0f;
        id[i].Transform[0][3] = (float)i;
        id[i].InstanceID = 50 + i;
        id[i].InstanceMask = 0xFF;
        id[i].InstanceContributionToHitGroupIndex = L.inst[i].contribution;
        id[i].AccelerationStructure = (L.inst[i].blas == kA ? blasA : blasB)->GetGPUVirtualAddress();
    }
    ib->Unmap(0, nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tl{};
    tl.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tl.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tl.NumDescs = n; tl.InstanceDescs = ib->GetGPUVirtualAddress();
    auto tlas = BuildAS(g, tl, keep);
    // --table: a second live scene, instance A at contribution 2, which puts
    // different geometries on records the real scene uses.
    ComPtr<ID3D12Resource> decoy;
    if (g_table) {
        auto db = Buffer(g.dev.Get(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_RAYTRACING_INSTANCE_DESC* dd = nullptr;
        HR(db->Map(0, nullptr, (void**)&dd), "Map decoy instances");
        std::memset(dd, 0, sizeof(*dd));
        dd->Transform[0][0] = dd->Transform[1][1] = dd->Transform[2][2] = 1.0f;
        dd->InstanceMask = 0xFF;
        dd->InstanceContributionToHitGroupIndex = 2;
        dd->AccelerationStructure = blasA->GetGPUVirtualAddress();
        db->Unmap(0, nullptr);
        keep.push_back(db);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS dl = tl;
        dl.NumDescs = 1; dl.InstanceDescs = db->GetGPUVirtualAddress();
        decoy = BuildAS(g, dl, keep);
    }
    g.flush();

    // Global: constants b0, TLAS t0, output u0. Local: one constant at
    // b0 space1, or three at b1 space1 for the second closest-hit.
    D3D12_ROOT_PARAMETER gp[3]{};
    gp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; gp[0].Constants.Num32BitValues = 4;
    gp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    // --table: t0 is the table's descriptor 2, so a wrong offset lands on a decoy.
    D3D12_DESCRIPTOR_RANGE tr{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 2 };
    if (g_table) {
        gp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        gp[1].DescriptorTable = { 1, &tr };
    }
    gp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC gd{ 3, gp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    auto grs = RootSig(g.dev.Get(), gd);
    D3D12_ROOT_PARAMETER lp1{}; lp1.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    lp1.Constants.Num32BitValues = 1; lp1.Constants.RegisterSpace = 1;
    D3D12_ROOT_SIGNATURE_DESC ld1{ 1, &lp1, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE };
    auto lrs1 = RootSig(g.dev.Get(), ld1);
    D3D12_ROOT_PARAMETER lp3{}; lp3.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    lp3.Constants.Num32BitValues = 3; lp3.Constants.ShaderRegister = 1; lp3.Constants.RegisterSpace = 1;
    D3D12_ROOT_SIGNATURE_DESC ld3{ 1, &lp3, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE };
    auto lrs3 = RootSig(g.dev.Get(), ld3);

    auto lib = Compile(L.R, L.M, L.anyhit);
    D3D12_DXIL_LIBRARY_DESC libd{};
    libd.DXILLibrary = { lib->GetBufferPointer(), lib->GetBufferSize() };
    D3D12_HIT_GROUP_DESC hg[3]{};
    const wchar_t* hgName[3] = { L"HitGroup", L"HitGroup3", L"HitGroupOther" };
    const wchar_t* chName[3] = { L"ClosestHit", L"ClosestHit3", L"ClosestHitOther" };
    for (int k = 0; k < 3; ++k) {
        hg[k].HitGroupExport = hgName[k];
        hg[k].Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg[k].ClosestHitShaderImport = chName[k];
        if (L.anyhit && k == 0) hg[k].AnyHitShaderImport = L"AnyHit";
    }
    D3D12_RAYTRACING_SHADER_CONFIG sc{ 16, 8 };
    D3D12_RAYTRACING_PIPELINE_CONFIG pc{ 1 };
    D3D12_GLOBAL_ROOT_SIGNATURE gsub{ grs.Get() };
    D3D12_LOCAL_ROOT_SIGNATURE lsub1{ lrs1.Get() }, lsub3{ lrs3.Get() };
    std::vector<D3D12_STATE_SUBOBJECT> sub;
    sub.reserve(16);
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libd });
    for (auto& h : hg) sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &h });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gsub });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsub1 });
    const D3D12_STATE_SUBOBJECT* l1 = &sub.back();
    const wchar_t* l1Exports[] = { L"HitGroup", L"HitGroupOther" };
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION a1{ l1, 2, l1Exports };
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &a1 });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsub3 });
    const D3D12_STATE_SUBOBJECT* l3 = &sub.back();
    const wchar_t* l3Exports[] = { L"HitGroup3" };
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION a3{ l3, 1, l3Exports };
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &a3 });
    D3D12_STATE_OBJECT_DESC sd{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)sub.size(), sub.data() };
    ComPtr<ID3D12StateObject> so;
    if (g_mode == 0) {
        HR(g.dev->CreateStateObject(&sd, IID_PPV_ARGS(&so)), "CreateStateObject");
    } else {
        // Unreal's way (D3D12RayTracing.cpp): every shader compiled into a
        // COLLECTION with its exports RENAMED and one local root signature
        // associated to each export by name, then collections linked into a
        // pipeline. --grow links the hit collection by AddToStateObject.
        D3D12_ROOT_SIGNATURE_DESC ld0{ 0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE };
        auto lrs0 = RootSig(g.dev.Get(), ld0);
        D3D12_LOCAL_ROOT_SIGNATURE lsub0{ lrs0.Get() };

        // Raygen and miss.
        D3D12_EXPORT_DESC rgExp[2] = { { L"RayGen", nullptr, D3D12_EXPORT_FLAG_NONE },
                                       { L"Miss", nullptr, D3D12_EXPORT_FLAG_NONE } };
        D3D12_DXIL_LIBRARY_DESC rgLib = libd;
        rgLib.NumExports = 2; rgLib.pExports = rgExp;
        std::vector<D3D12_STATE_SUBOBJECT> rs;
        rs.reserve(8);
        rs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &rgLib });
        rs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });
        rs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });
        rs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gsub });
        rs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsub0 });
        const D3D12_STATE_SUBOBJECT* r0 = &rs.back();
        const wchar_t* rgName[] = { L"RayGen" }; const wchar_t* msName[] = { L"Miss" };
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION ra{ r0, 1, rgName }, rm{ r0, 1, msName };
        rs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &ra });
        rs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &rm });
        D3D12_STATE_OBJECT_DESC rd{ D3D12_STATE_OBJECT_TYPE_COLLECTION, (UINT)rs.size(), rs.data() };
        ComPtr<ID3D12StateObject> rgColl;
        HR(g.dev->CreateStateObject(&rd, IID_PPV_ARGS(&rgColl)), "CreateStateObject(raygen collection)");

        // Hit shaders, renamed, and the hit groups over the new names.
        D3D12_EXPORT_DESC hExp[4] = {
            { L"CH_a", L"ClosestHit", D3D12_EXPORT_FLAG_NONE },
            { L"CH_b", L"ClosestHit3", D3D12_EXPORT_FLAG_NONE },
            { L"CH_c", L"ClosestHitOther", D3D12_EXPORT_FLAG_NONE },
            { L"AH_a", L"AnyHit", D3D12_EXPORT_FLAG_NONE } };
        D3D12_DXIL_LIBRARY_DESC hLib = libd;
        hLib.NumExports = 4; hLib.pExports = hExp;
        D3D12_HIT_GROUP_DESC chg[3] = { hg[0], hg[1], hg[2] };
        chg[0].ClosestHitShaderImport = L"CH_a";
        chg[1].ClosestHitShaderImport = L"CH_b";
        chg[2].ClosestHitShaderImport = L"CH_c";
        if (chg[0].AnyHitShaderImport) chg[0].AnyHitShaderImport = L"AH_a";
        std::vector<D3D12_STATE_SUBOBJECT> hs;
        hs.reserve(16);
        hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &hLib });
        for (auto& h : chg) hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &h });
        hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });
        hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });
        hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gsub });
        hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsub1 });
        const D3D12_STATE_SUBOBJECT* h1 = &hs.back();
        hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsub3 });
        const D3D12_STATE_SUBOBJECT* h3 = &hs.back();
        // One association per SHADER, as Unreal does it: the hit groups' own
        // names are never listed.
        const wchar_t* e1[] = { L"CH_a" }; const wchar_t* e2[] = { L"CH_c" };
        const wchar_t* e3[] = { L"AH_a" }; const wchar_t* e4[] = { L"CH_b" };
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION x1{ h1, 1, e1 }, x2{ h1, 1, e2 }, x3{ h1, 1, e3 },
                                               x4{ h3, 1, e4 };
        for (auto* x : { &x1, &x2, &x3, &x4 })
            hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, x });
        D3D12_STATE_OBJECT_DESC hd{ D3D12_STATE_OBJECT_TYPE_COLLECTION, (UINT)hs.size(), hs.data() };
        ComPtr<ID3D12StateObject> hitColl;
        HR(g.dev->CreateStateObject(&hd, IID_PPV_ARGS(&hitColl)), "CreateStateObject(hit collection)");

        D3D12_EXISTING_COLLECTION_DESC ec1{ rgColl.Get(), 0, nullptr }, ec2{ hitColl.Get(), 0, nullptr };
        D3D12_STATE_OBJECT_CONFIG grow{ D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS };
        std::vector<D3D12_STATE_SUBOBJECT> ps;
        ps.reserve(8);
        ps.push_back({ D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &ec1 });
        if (g_mode == 1) ps.push_back({ D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &ec2 });
        else ps.push_back({ D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &grow });
        ps.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });
        ps.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });
        ps.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gsub });
        D3D12_STATE_OBJECT_DESC pd{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)ps.size(), ps.data() };
        HR(g.dev->CreateStateObject(&pd, IID_PPV_ARGS(&so)), "CreateStateObject(pipeline)");
        if (g_mode == 2) {
            // The addition repeats the configs, as an addition must.
            std::vector<D3D12_STATE_SUBOBJECT> as;
            as.reserve(8);
            as.push_back({ D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &ec2 });
            as.push_back({ D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &grow });
            as.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });
            as.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });
            as.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gsub });
            D3D12_STATE_OBJECT_DESC addDesc{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)as.size(), as.data() };
            ComPtr<ID3D12Device7> d7;
            HR(g.dev.As(&d7), "ID3D12Device7");
            ComPtr<ID3D12StateObject> grown;
            HR(d7->AddToStateObject(&addDesc, so.Get(), IID_PPV_ARGS(&grown)), "AddToStateObject");
            so = grown;
        }
    }
    ComPtr<ID3D12StateObjectProperties> props;
    HR(so.As(&props), "StateObjectProperties");

    // Shader table: raygen at 0, miss at 64, hit records at 128, stride 64.
    // Each hit record: identifier, then its local root constants.
    const UINT64 idSz = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, stride = 64;
    const UINT64 offHit = 128, total = offHit + stride * L.records;
    auto table = Buffer(g.dev.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* t = nullptr; HR(table->Map(0, nullptr, (void**)&t), "Map table");
    std::memset(t, 0, (size_t)total);
    std::memcpy(t, props->GetShaderIdentifier(L"RayGen"), idSz);
    std::memcpy(t + 64, props->GetShaderIdentifier(L"Miss"), idSz);
    for (UINT i = 0; i < L.records; ++i) {
        uint8_t* rec = t + offHit + stride * i;
        std::memcpy(rec, props->GetShaderIdentifier(hgName[L.group[i]]), idSz);
        const uint32_t tags[3] = { 1000 + i, 2000 + i, 3000 + i };
        std::memcpy(rec + idSz, tags, L.group[i] == 1 ? 12 : 4);
    }
    table->Unmap(0, nullptr);

    const UINT W = kWPer * n;
    auto out = Buffer(g.dev.Get(), (UINT64)W * kH * 16, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g.cl->SetPipelineState1(so.Get());
    g.cl->SetComputeRootSignature(grs.Get());
    const uint32_t c[4] = { W, kH, n, 0 };
    g.cl->SetComputeRoot32BitConstants(0, 4, c, 0);
    ComPtr<ID3D12DescriptorHeap> heap, staging;
    if (g_table) {
        const UINT inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        HR(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; hd.NumDescriptors = 2;
        HR(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&staging)), "CreateDescriptorHeap(staging)");
        auto srv = [&](ID3D12Resource* as, D3D12_CPU_DESCRIPTOR_HANDLE at) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.RaytracingAccelerationStructure.Location = as->GetGPUVirtualAddress();
            g.dev->CreateShaderResourceView(nullptr, &sd, at);
        };
        auto at = [&](ID3D12DescriptorHeap* h, UINT i) {
            D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart();
            c.ptr += (SIZE_T)i * inc;
            return c;
        };
        // Staging: 0 decoy, 1 the real scene. Visible: the table starts at
        // 1, so t0 is slot 3; slots 1, 2 and 4 hold the decoy, and slot 3
        // holds a decoy written directly before the real one is copied over it.
        srv(decoy.Get(), at(staging.Get(), 0));
        srv(tlas.Get(), at(staging.Get(), 1));
        srv(decoy.Get(), at(heap.Get(), 3));
        g.dev->CopyDescriptorsSimple(1, at(heap.Get(), 1), at(staging.Get(), 0),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g.dev->CopyDescriptorsSimple(1, at(heap.Get(), 2), at(staging.Get(), 0),
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE dst[2] = { at(heap.Get(), 3), at(heap.Get(), 4) };
        D3D12_CPU_DESCRIPTOR_HANDLE src[2] = { at(staging.Get(), 1), at(staging.Get(), 0) };
        UINT ones[2] = { 1, 1 };
        g.dev->CopyDescriptors(2, dst, ones, 2, src, ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ID3D12DescriptorHeap* hs[] = { heap.Get() };
        g.cl->SetDescriptorHeaps(1, hs);
        D3D12_GPU_DESCRIPTOR_HANDLE tb = heap->GetGPUDescriptorHandleForHeapStart();
        tb.ptr += inc;
        g.cl->SetComputeRootDescriptorTable(1, tb);
    } else {
        g.cl->SetComputeRootShaderResourceView(1, tlas->GetGPUVirtualAddress());
    }
    g.cl->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
    D3D12_DISPATCH_RAYS_DESC dr{};
    const auto base = table->GetGPUVirtualAddress();
    dr.RayGenerationShaderRecord = { base, idSz };
    dr.MissShaderTable = { base + 64, idSz, idSz };
    dr.HitGroupTable = { base + offHit, stride * L.records, stride };
    dr.Width = W; dr.Height = kH; dr.Depth = 1;
    g.cl->DispatchRays(&dr);
    UavAll(g.cl.Get());

    auto rb = Buffer(g.dev.Get(), (UINT64)W * kH * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g.cl->ResourceBarrier(1, &b);
    g.cl->CopyBufferRegion(rb.Get(), 0, out.Get(), 0, (UINT64)W * kH * 16);
    g.flush();
    std::vector<uint32_t> img((size_t)W * kH * 4);
    void* m = nullptr; HR(rb->Map(0, nullptr, &m), "Map readback");
    std::memcpy(img.data(), m, img.size() * 4); rb->Unmap(0, nullptr);
    return img;
}

int main(int argc, char** argv) {
    std::set<std::string> want;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--sm66")) g_sm66 = true;
        else if (!std::strcmp(argv[i], "--collections")) g_mode = 1;
        else if (!std::strcmp(argv[i], "--grow")) g_mode = 2;
        else if (!std::strcmp(argv[i], "--table")) g_table = true;
        else want.insert(argv[i]);
    }
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { std::printf("no DXGI factory\n"); return 1; }
    ComPtr<IDXGIAdapter1> warp, hw;
    fac->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&hw));
    DXGI_ADAPTER_DESC1 hd{}; hw->GetDesc1(&hd);
    std::printf("GeometryIndex() in an application's DXR 1.0 hit shaders, %s%s, %s\n",
                g_sm66 ? "lib_6_6" : "lib_6_5", g_table ? ", scene through a descriptor table" : "",
                g_mode == 0 ? "one state object" : g_mode == 1 ? "collections"
                                                               : "collections grown by AddToStateObject");
    std::printf("ground truth WARP, against %ls\n\n", hd.Description);

    int failed = 0, ran = 0;
    for (const auto& L : Layouts()) {
        if (!want.empty() && !want.count(L.name)) continue;
        ++ran;
        std::vector<uint32_t> a, b;
        try { a = Run(warp.Get(), L); }
        catch (const std::exception& e) { std::printf("%-7s WARP FAILED: %s\n", L.name, e.what()); ++failed; continue; }
        try { b = Run(hw.Get(), L); }
        catch (const std::exception& e) { std::printf("%-7s DIVERGE: hardware failed: %s\n", L.name, e.what()); ++failed; continue; }
        size_t px = a.size() / 4, bad = 0, hits = 0, badG = 0, badRec = 0;
        std::set<uint32_t> gs;
        for (size_t i = 0; i < px; ++i) {
            if (a[4 * i] != 0xFFFFFFFF) { ++hits; gs.insert(a[4 * i]); }
            if (std::memcmp(&a[4 * i], &b[4 * i], 16)) {
                ++bad;
                if (a[4 * i] != b[4 * i]) ++badG;
                if (a[4 * i + 3] != b[4 * i + 3]) ++badRec;
            }
        }
        // The layout has to be able to fail: WARP must see several geometries.
        const bool sensitive = gs.size() >= 2;
        const bool ok = bad == 0 && sensitive;
        if (!ok) ++failed;
        std::printf("%-7s %s  %zu of %zu pixels hit, %zu distinct geometry indices, %zu differ "
                    "(geometry %zu, record data %zu)%s\n",
                    L.name, ok ? "MATCH  " : "DIVERGE", hits, px, gs.size(), bad, badG, badRec,
                    sensitive ? "" : "  NOT SENSITIVE, WARP saw one geometry");
    }
    std::printf("\n%s\n", failed ? "FAILED" : (ran ? "ALL MATCH" : "no layout matched the arguments"));
    return failed ? 1 : 0;
}
