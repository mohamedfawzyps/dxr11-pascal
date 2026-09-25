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
// --bindless traces ResourceDescriptorHeap[i] instead, Unreal's bindless
// form (lib_6_6), with i in a root CBV in upload memory as Unreal binds its
// uniform buffers; --bindlessrc puts i in root constants. The same heap and
// decoys as --table, and the decoy scene bound as a root SRV beside it, so a
// shim that took a bound root SRV for the scene draws the wrong one.
//
// --indirect issues the dispatch through ExecuteIndirect with the arguments
// in upload memory; --indirectgpu with them in GPU memory, copied there on
// the GPU, which the shim serves by splitting the command list.
//
// --libassoc declares the local root signatures and their associations IN
// THE LIBRARY (HLSL LocalRootSignature and SubobjectToExportsAssociation)
// instead of as state object subobjects. Through collections, the library is
// included with an EXPORT LIST, which then has to name those subobjects (the
// runtime includes a library's subobject only when a list names it).
// --libhg declares the hit groups in the library too. --libsplit declares
// the signatures in a SECOND library, associated from the first.
// --dxilassoc: the signatures in a second library, associated by the state
// object by name (DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION); --dxildefault with
// LrsRec as the state object's DEFAULT, so it reaches the raygen and miss.
//
// --gpuinst puts the instance descriptions in GPU memory, copied there on the
// GPU, as Unreal writes them: the shim cannot read them before the dispatch.
// --stale (implies --gpuinst) first builds the scene with every instance's
// structure swapped, submits, then builds it again IN PLACE with the real
// ones, so what the shim read is the older build's scene.
//
// --deserialize puts every A instance on a DESERIALIZED copy of A, as Unreal
// loads offline structures, and then rebuilds A's own address with B's two
// geometries, as a streaming engine reuses memory. So no structure the shim
// knows has four geometries any more. A deserialized structure's geometry is
// driver-opaque, so the shim must refuse, by name, every layout reaching it.
//
// --localscene binds the scene through the RAYGEN's LOCAL root signature, a
// root SRV at t0 space2 in its shader record; --localscenetable through a
// descriptor table there, over the same heap and decoys as --table. The
// global root SRV holds the conflicting decoy scene, so a shim that resolves
// the scene anywhere else draws the wrong one, and one that cannot resolve it
// judges both scenes live and refuses.
//
// --gpusbt puts the shader table in GPU memory, copied there on the GPU, so
// the raygen record cannot be read on the CPU at record time.
//
// --recurse makes the pipeline's recursion depth 2 and traces from the hit
// and miss shaders too: each closest-hit that reads GeometryIndex() traces a
// second ray beside its hit and folds that ray's geometry and record into
// its own answer, and the bottom half of the image starts its rays off the
// scene, so the miss shader traces the real ray. The shim has to give those
// TraceRay calls the same scene and records as the raygen's. With
// --localscene too, the hit groups' and the miss's local root signatures
// carry the scene as well, so it has to be read from their records.
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
static int g_bindless = 0;   // 1 index in a root CBV, 2 in root constants
static int g_indirect = 0;   // 1 arguments in upload memory, 2 in GPU memory
// 0 none, 1 --libassoc, 2 --libhg, 3 --libsplit, 4 --dxilassoc, 5 --dxildefault
static int g_libassoc = 0;
static bool g_gpuinst = false, g_stale = false, g_deserialize = false;
static int g_localScene = 0;   // 1 a root SRV in the raygen's record, 2 a descriptor table
static bool g_gpuSbt = false;
static bool g_recurse = false;
// --warpglobal: WARP draws the ground truth with the scene through the global
// root SRV instead of the heap. Measured: WARP removes its device when a
// closest-hit or miss traces a scene from ResourceDescriptorHeap (the raygen
// alone works, and the debug layer says nothing), so --recurse --bindless
// needs it. How the scene is bound changes nothing the shaders write.
static bool g_warpGlobal = false;
// --dynargs: every TraceRay's RayContributionToHitGroupIndex and multiplier
// computed at run time, from the constants (MDyn) and the ray index times a
// constant (RDyn * (x & 1)), as an application choosing ray types does. The
// layout "perray" then puts odd columns on the next record. --norefine: the
// shim reads no constant at the dispatch, so every pair counts (its widest
// layout, several scene structures).
static bool g_dynArgs = false;
// --twoscenes: the raygen traces the scene, then a SECOND scene (t1, the same
// instances in reverse order) and folds both answers into its output; with
// --recurse the closest-hits trace the second scene. With --localscene the
// hit records carry their scene, alternately the first and the second, and
// the miss record the second: the scene is the RECORD's. --twoconflict: the
// second scene is one instance of B at contribution 1 instead (where the
// layout has 3 records or more), so the two scenes put different geometries
// on one record and no table labelled for both can serve them.
static int g_twoScenes = 0;
static int g_warpOnly = 0;
static bool g_withHw = false;

// --- the application's shaders ------------------------------------------------
// M_VAL, R_VAL: the TraceRay multiplier and ray contribution, literal as in a
// real engine. ANYHIT: the any-hit variant. Two closest-hits, because mixrs
// needs a second local root signature layout.
static const char* kLib = R"HLSL(
#if DYNARGS
cbuffer CB : register(b0) { uint W; uint H; uint NInst; uint Pad; uint RDyn; uint MDyn; };
#else
cbuffer CB : register(b0) { uint W; uint H; uint NInst; uint Pad; };
#endif
#if LOCALSCENE
RaytracingAccelerationStructure Scene : register(t0, space2);
#elif !BINDLESS
RaytracingAccelerationStructure Scene : register(t0);
#endif
RWStructuredBuffer<uint4> Out : register(u0);
#if TWOSCENES
RaytracingAccelerationStructure Scene2 : register(t1);
#endif

cbuffer Rec : register(b0, space1) { uint RecTag; };
cbuffer Rec3 : register(b1, space1) { uint RecA; uint RecB; uint RecC; };
#if LIBASSOC == 1 || LIBASSOC == 2
LocalRootSignature LrsRec = { "RootConstants(num32BitConstants=1, b0, space=1)" };
LocalRootSignature LrsRec3 = { "RootConstants(num32BitConstants=3, b1, space=1)" };
#endif
#if LIBASSOC >= 1 && LIBASSOC <= 3
SubobjectToExportsAssociation AssocRec = { "LrsRec", "HitGroup;HitGroupOther" };
SubobjectToExportsAssociation AssocRec3 = { "LrsRec3", "HitGroup3" };
#endif
#if LIBASSOC == 2
#if ANYHIT
TriangleHitGroup HitGroup = { "AnyHit", "ClosestHit" };
#else
TriangleHitGroup HitGroup = { "", "ClosestHit" };
#endif
TriangleHitGroup HitGroup3 = { "", "ClosestHit3" };
TriangleHitGroup HitGroupOther = { "", "ClosestHitOther" };
#endif

struct Pay {
    uint4 v;
#if RECURSE
    uint depth;
#endif
};

#if BINDLESS
#define SCENE_DECL RaytracingAccelerationStructure Scene = ResourceDescriptorHeap[Pad];
#else
#define SCENE_DECL
#endif
#if ANYHIT
#define TRACE_FLAGS RAY_FLAG_NONE
#else
#define TRACE_FLAGS RAY_FLAG_FORCE_OPAQUE
#endif
#define NO_HIT uint4(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)

RayDesc MakeRay(uint2 p) {
    RayDesc r;
    r.Origin = float3((p.x + 0.5) / W * NInst, (p.y + 0.5) / H, 1.0);
    r.Direction = float3(0, 0, -1);
    r.TMin = 0.0; r.TMax = 10.0;
    return r;
}

[shader("raygeneration")] void RayGen() {
    uint2 p = DispatchRaysIndex().xy;
    SCENE_DECL
    RayDesc r = MakeRay(p);
    Pay pay; pay.v = NO_HIT;
#if RECURSE
    pay.depth = 0;
    if (p.y >= H / 2) r.Origin.x = -5.0;   // misses: the miss shader traces it
#endif
    TraceRay(Scene, TRACE_FLAGS, 0xFF, R_VAL, M_VAL, 0, r, pay);
#if TWOSCENES
    Pay q; q.v = NO_HIT;
#if RECURSE
    q.depth = 1;
#endif
    TraceRay(Scene2, TRACE_FLAGS, 0xFF, R_VAL, M_VAL, 0, MakeRay(p), q);
    pay.v.x += 256 * (q.v.x & 0xFF);
    pay.v.w += 65536 * (q.v.w & 0xFFFF);
#endif
    Out[p.y * W + p.x] = pay.v;
}

#if RECURSE
// A second ray from beside the hit, traced from the closest-hit; its geometry
// and record folded into this hit's answer.
void Follow(inout Pay p) {
    if (p.depth != 0) return;
    SCENE_DECL
    float3 h = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    RayDesc r;
    r.Origin = float3(h.x + 0.3, h.y, 1.0);
    r.Direction = float3(0, 0, -1);
    r.TMin = 0.0; r.TMax = 10.0;
    Pay q; q.v = NO_HIT; q.depth = 1;
#if TWOSCENES && !LOCALSCENE
    TraceRay(Scene2, TRACE_FLAGS, 0xFF, R_VAL, M_VAL, 0, r, q);
#else
    TraceRay(Scene, TRACE_FLAGS, 0xFF, R_VAL, M_VAL, 0, r, q);
#endif
    p.v.x += 16 * (q.v.x & 0xFF);
    p.v.w += 65536 * (q.v.w & 0xFFFF);
}
#endif

[shader("miss")] void Miss(inout Pay p) {
#if RECURSE
    if (p.depth == 0) {
        SCENE_DECL
        Pay q; q.v = NO_HIT; q.depth = 1;
        TraceRay(Scene, TRACE_FLAGS, 0xFF, R_VAL, M_VAL, 0, MakeRay(DispatchRaysIndex().xy), q);
        p.v = q.v + uint4(0, 0, 0, 5000);
        return;
    }
#endif
    p.v = NO_HIT;
}

[shader("closesthit")]
void ClosestHit(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    p.v = uint4(GeometryIndex(), InstanceIndex(), PrimitiveIndex(), RecTag);
#if RECURSE
    Follow(p);
#endif
}
[shader("closesthit")]
void ClosestHit3(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    p.v = uint4(GeometryIndex() + 100, InstanceIndex(), PrimitiveIndex(), RecA + RecB * 7 + RecC * 31);
#if RECURSE
    Follow(p);
#endif
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

// --libsplit, --dxilassoc: the local root signatures in a library of their own.
static const char* kLrsLib = R"HLSL(
LocalRootSignature LrsRec = { "RootConstants(num32BitConstants=1, b0, space=1)" };
LocalRootSignature LrsRec3 = { "RootConstants(num32BitConstants=3, b1, space=1)" };
)HLSL";

static ComPtr<IDxcBlob> Compile(int R, int M, bool anyhit, const char* src = kLib) {
    if (g_dynArgs) R = M = -1;   // computed at run time, see kLib
    static HMODULE m = LoadLibraryW(L"dxcompiler.dll");
    if (!m) throw std::runtime_error("dxcompiler.dll not found");
    auto create = (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance");
    ComPtr<IDxcCompiler3> c;
    HR(create(CLSID_DxcCompiler, IID_PPV_ARGS(&c)), "create IDxcCompiler3");
    std::wstring dr = L"R_VAL=" + std::to_wstring(R), dm = L"M_VAL=" + std::to_wstring(M);
    if (g_dynArgs) {
        dr = L"R_VAL=(RDyn*(DispatchRaysIndex().x&1))";
        dm = L"M_VAL=MDyn";
    }
    const wchar_t* dd = g_dynArgs ? L"DYNARGS=1" : L"DYNARGS=0";
    const wchar_t* d2 = g_twoScenes ? L"TWOSCENES=1" : L"TWOSCENES=0";
    std::wstring da = std::wstring(L"ANYHIT=") + (anyhit ? L"1" : L"0");
    const wchar_t* db = g_bindless ? L"BINDLESS=1" : L"BINDLESS=0";
    const std::wstring dls = L"LIBASSOC=" + std::to_wstring(g_libassoc);
    const wchar_t* dl = dls.c_str();
    const wchar_t* ds = g_localScene ? L"LOCALSCENE=1" : L"LOCALSCENE=0";
    const wchar_t* dc = g_recurse ? L"RECURSE=1" : L"RECURSE=0";
    std::vector<const wchar_t*> args = { L"-T", g_sm66 ? L"lib_6_6" : L"lib_6_5",
                                         L"-D", dr.c_str(), L"-D", dm.c_str(), L"-D", da.c_str(),
                                         L"-D", db, L"-D", dl, L"-D", ds, L"-D", dc,
                                         L"-D", dd, L"-D", d2 };
    DxcBuffer buf{ src, std::strlen(src), DXC_CP_UTF8 };
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
    // --dynargs only: R is 1 on odd columns, M 0, so every geometry of an
    // instance shares a record and the odd columns take the next one.
    if (g_dynArgs) v.push_back({ "perray", { {kA, 0}, {kB, 2} }, 0, 0, 4, false, {} });
    for (auto& l : v) {
        l.group.assign(l.records, 0);
        if (!std::strcmp(l.name, "slots"))
            for (UINT i = 1; i < l.records; i += 2) l.group[i] = 2;
        if (!std::strcmp(l.name, "mixrs") || !std::strcmp(l.name, "perray"))
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
        // GITEST_DEBUG=1: the D3D12 debug layer, its messages printed as they
        // happen, so a failing call says why.
        const bool dbg = std::getenv("GITEST_DEBUG") != nullptr;
        if (dbg) {
            ComPtr<ID3D12Debug> d;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) d->EnableDebugLayer();
        }
        HR(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)), "D3D12CreateDevice");
        if (dbg) {
            ComPtr<ID3D12InfoQueue1> iq;
            DWORD cookie = 0;
            if (SUCCEEDED(dev.As(&iq)))
                iq->RegisterMessageCallback(
                    [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID,
                       LPCSTR text, void*) {
                        if (sev <= D3D12_MESSAGE_SEVERITY_WARNING) std::printf("  [debug] %s\n", text);
                    }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        }
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
                                      std::vector<ComPtr<ID3D12Resource>>& keep,
                                      ID3D12Resource* into = nullptr) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    g.dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
    if (!info.ResultDataMaxSizeInBytes) throw std::runtime_error("prebuild info zero");
    auto scratch = Buffer(g.dev.Get(), info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    ComPtr<ID3D12Resource> as = into;
    if (!as)
        as = Buffer(g.dev.Get(), info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
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
                                        std::vector<ComPtr<ID3D12Resource>>& keep,
                                        ID3D12Resource* into = nullptr) {
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
    return BuildAS(g, in, keep, into);
}

// --deserialize: `src` serialized and deserialized into a new structure.
static ComPtr<ID3D12Resource> Deserialized(Gpu& g, ID3D12Resource* src,
                                           std::vector<ComPtr<ID3D12Resource>>& keep) {
    const D3D12_GPU_VIRTUAL_ADDRESS a = src->GetGPUVirtualAddress();
    auto info = Buffer(g.dev.Get(), 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pd{};
    pd.DestBuffer = info->GetGPUVirtualAddress();
    pd.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION;
    g.cl->EmitRaytracingAccelerationStructurePostbuildInfo(&pd, 1, &a);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = info.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g.cl->ResourceBarrier(1, &b);
    auto rb = Buffer(g.dev.Get(), 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    g.cl->CopyBufferRegion(rb.Get(), 0, info.Get(), 0, 256);
    g.flush();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC sd{};
    void* m = nullptr; HR(rb->Map(0, nullptr, &m), "Map serialization info");
    std::memcpy(&sd, m, sizeof(sd)); rb->Unmap(0, nullptr);
    if (!sd.SerializedSizeInBytes) throw std::runtime_error("serialized size 0");
    auto ser = Buffer(g.dev.Get(), sd.SerializedSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    g.cl->CopyRaytracingAccelerationStructure(ser->GetGPUVirtualAddress(), a,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
    b.Transition.pResource = ser.Get();
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    g.cl->ResourceBarrier(1, &b);
    auto out = Buffer(g.dev.Get(), src->GetDesc().Width, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
    g.cl->CopyRaytracingAccelerationStructure(out->GetGPUVirtualAddress(), ser->GetGPUVirtualAddress(),
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
    UavAll(g.cl.Get());
    g.flush();
    keep.push_back(info); keep.push_back(rb); keep.push_back(ser);
    return out;
}

static ComPtr<ID3D12RootSignature> RootSig(ID3D12Device* dev, const D3D12_ROOT_SIGNATURE_DESC& d) {
    ComPtr<ID3DBlob> b, e;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v{};
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    v.Desc_1_0 = d;
    HR(D3D12SerializeVersionedRootSignature(&v, &b, &e), "serialize RS");
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
    if (g_deserialize) {
        auto copy = Deserialized(g, blasA.Get(), keep);
        BuildBlas(g, kB, !L.anyhit, keep, blasA.Get());   // A's address, reused
        keep.push_back(blasA);
        blasA = copy;
    }
    const UINT n = (UINT)L.inst.size();
    const UINT64 ibBytes = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * n;
    // `swap`: every instance on the other structure, for --stale.
    auto instances = [&](bool swap) {
        auto ib = Buffer(g.dev.Get(), ibBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_RAYTRACING_INSTANCE_DESC* id = nullptr;
        HR(ib->Map(0, nullptr, (void**)&id), "Map instances");
        for (UINT i = 0; i < n; ++i) {
            std::memset(&id[i], 0, sizeof(id[i]));
            id[i].Transform[0][0] = id[i].Transform[1][1] = id[i].Transform[2][2] = 1.0f;
            id[i].Transform[0][3] = (float)i;
            id[i].InstanceID = 50 + i;
            id[i].InstanceMask = 0xFF;
            id[i].InstanceContributionToHitGroupIndex = L.inst[i].contribution;
            id[i].AccelerationStructure =
                ((L.inst[i].blas == kA) != swap ? blasA : blasB)->GetGPUVirtualAddress();
        }
        ib->Unmap(0, nullptr);
        keep.push_back(ib);
        if (!g_gpuinst) return ib->GetGPUVirtualAddress();
        // --gpuinst: copied into GPU memory on the GPU.
        auto gb = Buffer(g.dev.Get(), ibBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        g.cl->CopyBufferRegion(gb.Get(), 0, ib.Get(), 0, ibBytes);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = gb.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g.cl->ResourceBarrier(1, &b);
        keep.push_back(gb);
        return gb->GetGPUVirtualAddress();
    };
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tl{};
    tl.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tl.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tl.NumDescs = n; tl.InstanceDescs = instances(false);
    ComPtr<ID3D12Resource> tlas;
    if (g_stale) {
        // The older scene, submitted, then the real one built over it.
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS old = tl;
        old.InstanceDescs = instances(true);
        tlas = BuildAS(g, old, keep);
        g.flush();
        BuildAS(g, tl, keep, tlas.Get());
    } else {
        tlas = BuildAS(g, tl, keep);
    }
    // --twoscenes: the same instances in reverse order, so every position
    // hits another structure on other records.
    ComPtr<ID3D12Resource> scene2;
    if (g_twoScenes) {
        auto sb = Buffer(g.dev.Get(), ibBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_RAYTRACING_INSTANCE_DESC* sd = nullptr;
        HR(sb->Map(0, nullptr, (void**)&sd), "Map second scene instances");
        const bool conflict = g_twoScenes == 2 && L.records >= 3;
        const UINT n2 = conflict ? 1u : n;
        for (UINT i = 0; i < n2; ++i) {
            Inst x = L.inst[n - 1 - i];
            if (conflict) { x.blas = kB; x.contribution = 1; }
            std::memset(&sd[i], 0, sizeof(sd[i]));
            sd[i].Transform[0][0] = sd[i].Transform[1][1] = sd[i].Transform[2][2] = 1.0f;
            sd[i].Transform[0][3] = (float)i;
            sd[i].InstanceID = 70 + i;
            sd[i].InstanceMask = 0xFF;
            sd[i].InstanceContributionToHitGroupIndex = x.contribution;
            sd[i].AccelerationStructure = (x.blas == kA ? blasA : blasB)->GetGPUVirtualAddress();
        }
        sb->Unmap(0, nullptr);
        keep.push_back(sb);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS sl = tl;
        sl.NumDescs = n2;
        sl.InstanceDescs = sb->GetGPUVirtualAddress();
        scene2 = BuildAS(g, sl, keep);
    }
    // --table: a second live scene, instance A at contribution 2, which puts
    // different geometries on records the real scene uses.
    ComPtr<ID3D12Resource> decoy;
    if (g_table || g_bindless || g_localScene) {
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
    D3D12_ROOT_PARAMETER gp[4]{};
    gp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    gp[0].Constants.Num32BitValues = g_dynArgs ? 6 : 4;
    if (g_bindless == 1) gp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    gp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    // --table: t0 is the table's descriptor 2, so a wrong offset lands on a decoy.
    D3D12_DESCRIPTOR_RANGE tr{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 2 };
    if (g_table) {
        gp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        gp[1].DescriptorTable = { 1, &tr };
    }
    gp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    gp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    gp[3].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC gd{ g_twoScenes ? 4u : 3u, gp, 0, nullptr,
        g_bindless ? D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
                   : D3D12_ROOT_SIGNATURE_FLAG_NONE };
    auto grs = RootSig(g.dev.Get(), gd);
    // --recurse --localscene: the hit groups trace too, so their signatures
    // carry the scene after their constants.
    const bool hitScene = g_recurse && g_localScene;
    D3D12_ROOT_PARAMETER lp1[2]{}; lp1[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    lp1[0].Constants.Num32BitValues = 1; lp1[0].Constants.RegisterSpace = 1;
    D3D12_ROOT_PARAMETER lp3[2]{}; lp3[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    lp3[0].Constants.Num32BitValues = 3; lp3[0].Constants.ShaderRegister = 1; lp3[0].Constants.RegisterSpace = 1;
    D3D12_DESCRIPTOR_RANGE hsr{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2, 2 };
    for (D3D12_ROOT_PARAMETER* q : { &lp1[1], &lp3[1] }) {
        if (g_localScene == 2) {
            q->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            q->DescriptorTable = { 1, &hsr };
        } else {
            q->ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            q->Descriptor.RegisterSpace = 2;
        }
    }
    D3D12_ROOT_SIGNATURE_DESC ld1{ hitScene ? 2u : 1u, lp1, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE };
    auto lrs1 = RootSig(g.dev.Get(), ld1);
    D3D12_ROOT_SIGNATURE_DESC ld3{ hitScene ? 2u : 1u, lp3, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE };
    auto lrs3 = RootSig(g.dev.Get(), ld3);
    // --localscene: the raygen's own signature carries the scene, t0 space2,
    // as the table's descriptor 2 in --localscenetable.
    D3D12_ROOT_PARAMETER lps{};
    D3D12_DESCRIPTOR_RANGE lsr{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2, 2 };
    if (g_localScene == 2) {
        lps.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        lps.DescriptorTable = { 1, &lsr };
    } else {
        lps.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        lps.Descriptor.RegisterSpace = 2;
    }
    D3D12_ROOT_SIGNATURE_DESC lds{ 1, &lps, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE };
    auto lrsS = RootSig(g.dev.Get(), lds);
    D3D12_LOCAL_ROOT_SIGNATURE lsubS{ lrsS.Get() };
    const wchar_t* sExports[] = { L"RayGen", L"Miss" };

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
    D3D12_RAYTRACING_SHADER_CONFIG sc{ g_recurse ? 20u : 16u, 8 };
    D3D12_RAYTRACING_PIPELINE_CONFIG pc{ g_recurse ? 2u : 1u };
    D3D12_GLOBAL_ROOT_SIGNATURE gsub{ grs.Get() };
    D3D12_LOCAL_ROOT_SIGNATURE lsub1{ lrs1.Get() }, lsub3{ lrs3.Get() };
    // --libsplit, --dxilassoc: the signatures' own library.
    ComPtr<IDxcBlob> lrsLib;
    D3D12_DXIL_LIBRARY_DESC lrsd{};
    if (g_libassoc >= 3) {
        lrsLib = Compile(L.R, L.M, L.anyhit, kLrsLib);
        lrsd.DXILLibrary = { lrsLib->GetBufferPointer(), lrsLib->GetBufferSize() };
    }
    // --dxilassoc, --dxildefault: the state object associates them by name.
    const wchar_t* dx1[] = { L"HitGroup", L"HitGroupOther" };
    const wchar_t* dx3[] = { L"HitGroup3", L"ClosestHit3" };
    D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION da1{ L"LrsRec", g_libassoc == 5 ? 0u : 2u,
                                                     g_libassoc == 5 ? nullptr : dx1 },
                                                da3{ L"LrsRec3", g_libassoc == 5 ? 2u : 1u, dx3 };
    std::vector<D3D12_STATE_SUBOBJECT> sub;
    sub.reserve(16);
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libd });
    if (g_libassoc >= 3) sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lrsd });
    if (g_libassoc >= 4) {
        sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &da1 });
        sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &da3 });
    }
    if (g_libassoc != 2)
        for (auto& h : hg) sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &h });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gsub });
    // --libassoc: the library carries these itself.
    const wchar_t* l1Exports[] = { L"HitGroup", L"HitGroupOther" };
    const wchar_t* l3Exports[] = { L"HitGroup3" };
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION a1{ nullptr, 2, l1Exports }, a3{ nullptr, 1, l3Exports };
    if (!g_libassoc) {
        sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsub1 });
        a1.pSubobjectToAssociate = &sub.back();
        sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &a1 });
        sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsub3 });
        a3.pSubobjectToAssociate = &sub.back();
        sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &a3 });
    }
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION aS{ nullptr, hitScene ? 2u : 1u, sExports };
    if (g_localScene) {
        sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsubS });
        aS.pSubobjectToAssociate = &sub.back();
        sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &aS });
    }
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
        const D3D12_STATE_SUBOBJECT* rS = r0;
        if (g_localScene) {
            rs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsubS });
            rS = &rs.back();
        }
        const wchar_t* rgName[] = { L"RayGen" }; const wchar_t* msName[] = { L"Miss" };
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION ra{ rS, 1, rgName }, rm{ hitScene ? rS : r0, 1, msName };
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
        std::vector<D3D12_EXPORT_DESC> hList(hExp, hExp + 4);
        auto name = [](const wchar_t* w) { return D3D12_EXPORT_DESC{ w, nullptr, D3D12_EXPORT_FLAG_NONE }; };
        if (g_libassoc == 2) {
            // The library's hit groups import the shaders by their own
            // names, and nothing follows a rename.
            hList.clear();
            for (const wchar_t* w : { L"ClosestHit", L"ClosestHit3", L"ClosestHitOther", L"AnyHit",
                                      L"HitGroup", L"HitGroup3", L"HitGroupOther" })
                hList.push_back(name(w));
        }
        // The subobjects the library declares are included only by name.
        if (g_libassoc == 1 || g_libassoc == 2)
            for (const wchar_t* w : { L"LrsRec", L"LrsRec3" }) hList.push_back(name(w));
        if (g_libassoc >= 1 && g_libassoc <= 3)
            for (const wchar_t* w : { L"AssocRec", L"AssocRec3" }) hList.push_back(name(w));
        D3D12_DXIL_LIBRARY_DESC hLib = libd;
        hLib.NumExports = (UINT)hList.size(); hLib.pExports = hList.data();
        const wchar_t* cx1[] = { L"HitGroup", L"HitGroupOther" };
        const wchar_t* cx3[] = { L"HitGroup3", L"CH_b" };
        D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION ca1{ L"LrsRec", g_libassoc == 5 ? 0u : 2u,
                                                         g_libassoc == 5 ? nullptr : cx1 },
                                                    ca3{ L"LrsRec3", g_libassoc == 5 ? 2u : 1u, cx3 };
        D3D12_HIT_GROUP_DESC chg[3] = { hg[0], hg[1], hg[2] };
        chg[0].ClosestHitShaderImport = L"CH_a";
        chg[1].ClosestHitShaderImport = L"CH_b";
        chg[2].ClosestHitShaderImport = L"CH_c";
        if (chg[0].AnyHitShaderImport) chg[0].AnyHitShaderImport = L"AH_a";
        std::vector<D3D12_STATE_SUBOBJECT> hs;
        hs.reserve(16);
        hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &hLib });
        if (g_libassoc >= 3) hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lrsd });
        if (g_libassoc >= 4) {
            hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &ca1 });
            hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &ca3 });
        }
        if (g_libassoc != 2)
            for (auto& h : chg) hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &h });
        hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });
        hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });
        hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gsub });
        // One association per SHADER, as Unreal does it: the hit groups' own
        // names are never listed.
        const wchar_t* e1[] = { L"CH_a" }; const wchar_t* e2[] = { L"CH_c" };
        const wchar_t* e3[] = { L"AH_a" }; const wchar_t* e4[] = { L"CH_b" };
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION x1{ nullptr, 1, e1 }, x2{ nullptr, 1, e2 },
                                               x3{ nullptr, 1, e3 }, x4{ nullptr, 1, e4 };
        if (!g_libassoc) {
            hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsub1 });
            const D3D12_STATE_SUBOBJECT* h1 = &hs.back();
            hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsub3 });
            const D3D12_STATE_SUBOBJECT* h3 = &hs.back();
            x1.pSubobjectToAssociate = x2.pSubobjectToAssociate = x3.pSubobjectToAssociate = h1;
            x4.pSubobjectToAssociate = h3;
            for (auto* x : { &x1, &x2, &x3, &x4 })
                hs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, x });
        }
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
    ComPtr<ID3D12DescriptorHeap> heap, staging;
    const UINT inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (g_table || g_bindless || g_localScene == 2) {
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
    }
    g.cl->SetPipelineState1(so.Get());
    g.cl->SetComputeRootSignature(grs.Get());
    // --bindless: the scene is heap slot 3.
    // --dynargs: RDyn, MDyn after them, the layout's arguments.
    const uint32_t c[6] = { W, kH, n, g_bindless ? 3u : 0u,
                            std::strcmp(L.name, "perray") ? (uint32_t)L.R : 1u, (uint32_t)L.M };
    const UINT nc = g_dynArgs ? 6 : 4;
    ComPtr<ID3D12Resource> cb;
    if (g_bindless == 1) {
        cb = Buffer(g.dev.Get(), 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* cm = nullptr; HR(cb->Map(0, nullptr, &cm), "Map cb");
        std::memcpy(cm, c, sizeof(c)); cb->Unmap(0, nullptr);
        g.cl->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress());
    } else {
        g.cl->SetComputeRoot32BitConstants(0, nc, c, 0);
    }
    if (g_table) {
        D3D12_GPU_DESCRIPTOR_HANDLE tb = heap->GetGPUDescriptorHandleForHeapStart();
        tb.ptr += inc;
        g.cl->SetComputeRootDescriptorTable(1, tb);
    } else {
        g.cl->SetComputeRootShaderResourceView(
            1, (g_bindless || g_localScene ? decoy : tlas)->GetGPUVirtualAddress());
    }
    // --localscene: the scene's argument in the raygen record, after the
    // identifier: its address, or the table's start (slot 1, so t0 is slot 3).
    UINT64 rgBytes = g_libassoc == 5 && g_mode == 0 ? idSz + 4 : idSz;
    if (g_localScene) {
        uint64_t arg = tlas->GetGPUVirtualAddress();
        if (g_localScene == 2) arg = heap->GetGPUDescriptorHandleForHeapStart().ptr + inc;
        uint8_t* tm = nullptr; HR(table->Map(0, nullptr, (void**)&tm), "Map table");
        std::memcpy(tm + idSz, &arg, 8);
        if (hitScene) {
            // --twoscenes: the second scene's argument, in the same form.
            uint64_t arg2 = arg;
            if (g_twoScenes) arg2 = scene2->GetGPUVirtualAddress();
            std::memcpy(tm + 64 + idSz, &arg2, 8);
            for (UINT i = 0; i < L.records; ++i)   // after 1 or 3 constants, 8-aligned
                std::memcpy(tm + offHit + stride * i + (L.group[i] == 1 ? 48 : 40), i & 1 ? &arg2 : &arg, 8);
        }
        table->Unmap(0, nullptr);
        rgBytes = idSz + 8;
    }
    // --gpusbt: the table copied into GPU memory on the GPU.
    D3D12_GPU_VIRTUAL_ADDRESS tableAt = table->GetGPUVirtualAddress();
    if (g_gpuSbt) {
        auto gt = Buffer(g.dev.Get(), total, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        g.cl->CopyBufferRegion(gt.Get(), 0, table.Get(), 0, total);
        D3D12_RESOURCE_BARRIER tb{}; tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = gt.Get();
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g.cl->ResourceBarrier(1, &tb);
        keep.push_back(gt);
        tableAt = gt->GetGPUVirtualAddress();
    }
    g.cl->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
    if (g_twoScenes) g.cl->SetComputeRootShaderResourceView(3, scene2->GetGPUVirtualAddress());
    D3D12_DISPATCH_RAYS_DESC dr{};
    const auto base = tableAt;
    dr.RayGenerationShaderRecord = { base, rgBytes };
    dr.MissShaderTable = hitScene || (g_libassoc == 5 && g_mode == 0)
                             ? D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{ base + 64, 64, 64 }
                             : D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE{ base + 64, idSz, idSz };
    dr.HitGroupTable = { base + offHit, stride * L.records, stride };
    dr.Width = W; dr.Height = kH; dr.Depth = 1;
    ComPtr<ID3D12CommandSignature> sig;   // alive until the list has run
    if (!g_indirect) {
        g.cl->DispatchRays(&dr);
    } else {
        D3D12_INDIRECT_ARGUMENT_DESC ia{};
        ia.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
        D3D12_COMMAND_SIGNATURE_DESC csd{ sizeof(D3D12_DISPATCH_RAYS_DESC), 1, &ia, 0 };
        HR(g.dev->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&sig)), "CreateCommandSignature");
        auto up = Buffer(g.dev.Get(), sizeof(dr), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* am = nullptr; HR(up->Map(0, nullptr, &am), "Map args");
        std::memcpy(am, &dr, sizeof(dr)); up->Unmap(0, nullptr);
        keep.push_back(up);
        ID3D12Resource* args = up.Get();
        if (g_indirect == 2) {
            auto gpu = Buffer(g.dev.Get(), sizeof(dr), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
            g.cl->CopyBufferRegion(gpu.Get(), 0, up.Get(), 0, sizeof(dr));
            D3D12_RESOURCE_BARRIER ab{}; ab.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ab.Transition.pResource = gpu.Get();
            ab.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            ab.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            ab.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g.cl->ResourceBarrier(1, &ab);
            keep.push_back(gpu);
            args = gpu.Get();
        }
        g.cl->ExecuteIndirect(sig.Get(), 1, args, 0, nullptr, 0);
    }
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
        else if (!std::strcmp(argv[i], "--bindless")) g_bindless = 1, g_sm66 = true;
        else if (!std::strcmp(argv[i], "--bindlessrc")) g_bindless = 2, g_sm66 = true;
        else if (!std::strcmp(argv[i], "--indirect")) g_indirect = 1;
        else if (!std::strcmp(argv[i], "--indirectgpu")) g_indirect = 2;
        else if (!std::strcmp(argv[i], "--libassoc")) g_libassoc = 1;
        else if (!std::strcmp(argv[i], "--libhg")) g_libassoc = 2;
        else if (!std::strcmp(argv[i], "--libsplit")) g_libassoc = 3;
        else if (!std::strcmp(argv[i], "--dxilassoc")) g_libassoc = 4;
        else if (!std::strcmp(argv[i], "--dxildefault")) g_libassoc = 5;
        else if (!std::strcmp(argv[i], "--gpuinst")) g_gpuinst = true;
        else if (!std::strcmp(argv[i], "--stale")) g_stale = g_gpuinst = true;
        else if (!std::strcmp(argv[i], "--deserialize")) g_deserialize = true;
        else if (!std::strcmp(argv[i], "--localscene")) g_localScene = 1;
        else if (!std::strcmp(argv[i], "--localscenetable")) g_localScene = 2;
        else if (!std::strcmp(argv[i], "--gpusbt")) g_gpuSbt = true;
        else if (!std::strcmp(argv[i], "--recurse")) g_recurse = true;
        else if (!std::strcmp(argv[i], "--warpglobal")) g_warpGlobal = true;
        else if (!std::strcmp(argv[i], "--dynargs")) g_dynArgs = true;
        else if (!std::strcmp(argv[i], "--twoscenes")) g_twoScenes = 1;
        else if (!std::strcmp(argv[i], "--twoconflict")) g_twoScenes = 2;
        else if (!std::strcmp(argv[i], "--warponly") && i + 1 < argc) g_warpOnly = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--withhw")) g_withHw = true;
        else if (!std::strcmp(argv[i], "--norefine"))
            SetEnvironmentVariableA("DXR_TIER11_TARGS_NOREFINE", "1");
        // --perstruct N: N pairs per structure of the shim's scene copy, so a
        // few pairs already take several (DXR_TIER11_TARGS_PERSTRUCT).
        else if (!std::strcmp(argv[i], "--perstruct") && i + 1 < argc)
            SetEnvironmentVariableA("DXR_TIER11_TARGS_PERSTRUCT", argv[++i]);
        else want.insert(argv[i]);
    }
    if (g_twoScenes && (g_table || g_bindless || g_localScene == 2)) {
        std::printf("--twoscenes binds its second scene as a root SRV (or in root SRV records)\n");
        return 2;
    }
    if (g_localScene && (g_table || g_bindless || g_libassoc)) {
        std::printf("--localscene binds the scene one way only\n");
        return 2;
    }
    ComPtr<IDXGIFactory6> fac;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { std::printf("no DXGI factory\n"); return 1; }
    ComPtr<IDXGIAdapter1> warp, hw;
    fac->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&hw));
    DXGI_ADAPTER_DESC1 hd{}; hw->GetDesc1(&hd);
    std::printf("GeometryIndex() in an application's DXR 1.0 hit shaders, %s%s%s, %s\n",
                g_sm66 ? "lib_6_6" : "lib_6_5", g_table ? ", scene through a descriptor table"
                : g_bindless == 1 ? ", scene through the heap, index in a root CBV"
                : g_bindless == 2 ? ", scene through the heap, index in root constants" : "",
                g_indirect == 1 ? ", indirect (arguments in upload memory)"
                : g_indirect == 2 ? ", indirect (arguments in GPU memory)" : "",
                g_mode == 0 ? "one state object" : g_mode == 1 ? "collections"
                                                               : "collections grown by AddToStateObject");
    std::printf("ground truth WARP, against %ls\n\n", hd.Description);

    // --warponly N: the ground truth alone, N times per layout, each run
    // compared with the first. Measures whether WARP itself is stable, with
    // or without the shim beside it.
    if (g_warpOnly) {
        int moved = 0, runs = 0;
        // --withhw: one hardware run first, as the matrix has, so the shim
        // has wrapped a device and hooked the queues before WARP runs.
        if (g_withHw)
            try { Run(hw.Get(), Layouts()[0]); } catch (const std::exception&) {}
        for (const auto& L : Layouts()) {
            if (!want.empty() && !want.count(L.name)) continue;
            std::vector<uint32_t> first;
            for (int k = 0; k < g_warpOnly; ++k) {
                std::vector<uint32_t> a;
                try { a = Run(warp.Get(), L); } catch (const std::exception& e) {
                    std::printf("%-7s WARP FAILED: %s\n", L.name, e.what());
                    ++moved;
                    continue;
                }
                ++runs;
                if (first.empty()) { first = a; continue; }
                size_t d = 0, hits = 0;
                for (size_t i = 0; i < a.size() / 4; ++i) {
                    if (std::memcmp(&a[4 * i], &first[4 * i], 16)) ++d;
                    if (a[4 * i] != 0xFFFFFFFF) ++hits;
                }
                if (d) {
                    ++moved;
                    std::printf("%-7s run %d: %zu pixels hit, %zu differ from the first run\n", L.name, k, hits, d);
                }
            }
        }
        std::printf("\nWARP: %d runs, %d differ from their layout's first\n", runs, moved);
        return moved ? 1 : 0;
    }

    int failed = 0, ran = 0, unstable = 0;
    for (const auto& L : Layouts()) {
        if (!want.empty() && !want.count(L.name)) continue;
        ++ran;
        std::vector<uint32_t> a, b;
        const int keepBindless = g_bindless;
        if (g_warpGlobal) g_bindless = 0;
        bool warpOk = true;
        try { a = Run(warp.Get(), L); }
        catch (const std::exception& e) {
            std::printf("%-7s WARP FAILED: %s\n", L.name, e.what());
            warpOk = false;
        }
        g_bindless = keepBindless;
        if (!warpOk) { ++failed; continue; }
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
        // A divergence is checked against the ground truth itself: WARP once
        // more. Still counted as a failure, but it says which side moved.
        if (bad) {
            std::vector<uint32_t> a2;
            g_bindless = g_warpGlobal ? 0 : keepBindless;
            try { a2 = Run(warp.Get(), L); } catch (const std::exception&) {}
            g_bindless = keepBindless;
            size_t warpMoved = 0, hwVsSecond = 0, hits2 = 0;
            for (size_t i = 0; i < px && a2.size() == a.size(); ++i) {
                if (std::memcmp(&a[4 * i], &a2[4 * i], 16)) ++warpMoved;
                if (std::memcmp(&a2[4 * i], &b[4 * i], 16)) ++hwVsSecond;
                if (a2[4 * i] != 0xFFFFFFFF) ++hits2;
            }
            if (a2.size() != a.size())
                std::printf("        recheck: WARP's second run failed\n");
            else
                std::printf("        recheck: WARP again, %zu pixels hit, %zu differ from its first run; "
                            "the hardware differs from it in %zu%s\n",
                            hits2, warpMoved, hwVsSecond,
                            warpMoved && !hwVsSecond ? "  (WARP UNSTABLE: the ground truth moved, the "
                                                       "hardware matches its second run)" : "");
            // The two implementations agree exactly once WARP is run again:
            // the shim is not what moved. Counted apart, never hidden.
            if (a2.size() == a.size() && warpMoved && !hwVsSecond && sensitive) {
                --failed;
                ++unstable;
            }
        }
    }
    if (unstable)
        std::printf("\nGROUND TRUTH UNSTABLE in %d layout(s): WARP's first run differed from its "
                    "second, and the hardware matched the second exactly\n", unstable);
    std::printf("\n%s\n", failed ? "FAILED" : (ran ? "ALL MATCH" : "no layout matched the arguments"));
    return failed ? 1 : 0;
}
