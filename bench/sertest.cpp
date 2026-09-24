// SER test: can a software Shader Execution Reordering beat plain TraceRay on
// Pascal? An instrument, not a feature. Nothing here goes into the shim.
//
// SER regroups threads between finding a hit and running its hit shader, so a
// warp runs one material instead of many. Pascal has no hardware for that. The
// only DXR 1.0 way to do it is in software, in passes:
//
//   plain       one TraceRay, the closest-hit of the material that was hit
//               does the shading. What Unreal runs on this card today.
//   reordered   1. trace, each closest-hit only RECORDS its hit (material,
//                  primitive, t, barycentrics) and the raygen spills it
//               2. counting sort of the rays by material
//               3. a second raygen walks the sorted list and runs the
//                  material's shading as a CALLABLE shader, which is how an
//                  emulation would have to run a hit shader for a hit found
//                  earlier
//   twopass     the same as reordered without the sort. The control: it
//               separates what the split costs from what the sort buys.
//
// The scene is built to be the BEST case for reordering: a flat grid of tiny
// triangles, each assigned one of M materials, every material a different
// hit shader of equal, adjustable cost. With the `random` layout neighbouring
// triangles have unrelated materials, so every warp runs many hit shaders;
// the `blocked` layout gives the same materials in large patches, so a warp
// runs one. plain(random) - plain(blocked) is the divergence penalty, the most
// any reordering can win back. The spill is the minimum, 32 bytes a ray; a
// real engine's payload and live state are larger. So if reordering loses
// here, it loses everywhere. If it wins here, it only CAN win.
//
// Build:  build_sertest.bat   ->  benchout\sertest.exe
// Run:    benchout\sertest.exe [--m 32] [--res 1024] [--iters 15] [--k 8,32,128,512]
//
// Every configuration is also checked for correctness: the reordered and
// two-pass images must equal the plain one, and the sort must be a
// permutation with nondecreasing keys. A timing of a wrong answer is worthless.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>

using Microsoft::WRL::ComPtr;

#ifndef DXRTEST_AGILITY_VERSION
  #define DXRTEST_AGILITY_VERSION 619u
#endif
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = DXRTEST_AGILITY_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

static void Throw(const char* what, HRESULT hr) {
    char b[512];
    std::snprintf(b, sizeof(b), "%s failed, hr=0x%08X", what, (unsigned)hr);
    throw std::runtime_error(b);
}
static void HR(HRESULT hr, const char* what) { if (FAILED(hr)) Throw(what, hr); }

// --- settings ---------------------------------------------------------------
static uint32_t g_M = 32;          // materials, one hit shader each
static uint32_t g_res = 1024;      // rays per side, and grid quads per side
static int g_iters = 15;
static std::vector<uint32_t> g_ks = { 8, 32, 128, 512 };

// --- shaders ----------------------------------------------------------------
// Generated so every material is its own closest-hit and its own callable,
// with k a literal in each: M distinct shaders, equal cost.
static std::string MakeSource(uint32_t M) {
    std::string s = R"HLSL(
cbuffer CB : register(b0) { uint W; uint H; uint Mode; uint Iters; uint Unsorted; };
RaytracingAccelerationStructure Scene : register(t0);
struct HitRec { uint key; uint prim; float t; float bx; float by; uint p0; uint p1; uint p2; };
RWStructuredBuffer<float4> Out : register(u0);
RWStructuredBuffer<HitRec> Hits : register(u1);
RWStructuredBuffer<uint> Count : register(u2);
RWStructuredBuffer<uint> Sorted : register(u3);

struct Pay { float4 c; };
struct RecPay { uint key; uint prim; float t; float bx; float by; };
struct CallData { float4 c; uint prim; float t; float bx; float by; };

uint Hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float U(uint x) { return (float)(Hash(x) >> 8) * (1.0 / 16777216.0); }

float4 Shade(uint k, uint prim, float t, float2 b) {
    float3 c = float3(b.x, b.y, t) + float3(k * 0.013, prim * 1e-6, 0.5);
    float a = 1.1 + k * 0.071;
    [loop] for (uint i = 0; i < Iters; ++i)
        c = frac(sin(c * a + float3(i * 0.37, k * 0.11, 1.7)) * 437.585);
    return float4(c, (float)k);
}

RayDesc MakeRay(uint2 p) {
    RayDesc r; r.TMin = 0.0; r.TMax = 10.0;
    if (Mode == 0) {
        // Coherent: straight down, one ray per grid quad.
        r.Origin = float3((p.x + 0.5) / W * 2.0 - 1.0, (p.y + 0.5) / H * 2.0 - 1.0, 1.0);
        r.Direction = float3(0, 0, -1);
    } else {
        // Scattered: neighbouring rays go to unrelated places, as secondary
        // rays do. Divergent traversal as well as divergent shading.
        uint i = p.y * W + p.x;
        float3 o = float3(U(i * 4) * 2.0 - 1.0, U(i * 4 + 1) * 2.0 - 1.0, 1.0);
        float3 t = float3(U(i * 4 + 2) * 1.98 - 0.99, U(i * 4 + 3) * 1.98 - 0.99, 0.0);
        r.Origin = o; r.Direction = normalize(t - o);
    }
    return r;
}

// The hit table interleaves the two sets, record 2g the shading group of
// material g and 2g+1 its recording group. TraceRay uses only the low 4 bits
// of RayContributionToHitGroupIndex, so an offset of M would wrap to 0 and
// hang the GPU with the wrong payload type. It did, once.
[shader("raygeneration")] void RG_Plain() {
    uint2 p = DispatchRaysIndex().xy;
    Pay pay; pay.c = 0;
    TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 2, 0, MakeRay(p), pay);
    Out[p.y * W + p.x] = pay.c;
}
[shader("miss")] void MS_Plain(inout Pay p) { p.c = float4(0, 0, 0, -1); }

[shader("raygeneration")] void RG_Rec() {
    uint2 p = DispatchRaysIndex().xy;
    uint i = p.y * W + p.x;
    RecPay r; r.key = MKEY; r.prim = 0; r.t = 0; r.bx = 0; r.by = 0;
    TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 1, 2, 1, MakeRay(p), r);
    HitRec h; h.key = r.key; h.prim = r.prim; h.t = r.t; h.bx = r.bx; h.by = r.by;
    h.p0 = 0; h.p1 = 0; h.p2 = 0;
    Hits[i] = h;
    InterlockedAdd(Count[r.key], 1);
}
[shader("miss")] void MS_Rec(inout RecPay r) { r.key = MKEY; }

[shader("raygeneration")] void RG_Clear() {
    uint i = DispatchRaysIndex().x;
    if (i < 2 * (MKEY + 1)) Count[i] = 0;
}
[shader("raygeneration")] void RG_Scan() {
    uint sum = 0;
    for (uint k = 0; k <= MKEY; ++k) { Count[MKEY + 1 + k] = sum; sum += Count[k]; }
}
[shader("raygeneration")] void RG_Scatter() {
    uint2 p = DispatchRaysIndex().xy;
    uint i = p.y * W + p.x;
    uint slot;
    InterlockedAdd(Count[MKEY + 1 + Hits[i].key], 1, slot);
    Sorted[slot] = i;
}
[shader("raygeneration")] void RG_Shade() {
    uint2 p = DispatchRaysIndex().xy;
    uint j = p.y * W + p.x;
    uint idx = Unsorted ? j : Sorted[j];
    HitRec h = Hits[idx];
    float4 c = float4(0, 0, 0, -1);
    if (h.key < MKEY) {
        CallData d; d.c = 0; d.prim = h.prim; d.t = h.t; d.bx = h.bx; d.by = h.by;
        CallShader(h.key, d);
        c = d.c;
    }
    Out[idx] = c;
}
)HLSL";
    for (uint32_t k = 0; k < M; ++k) {
        char b[1024];
        std::snprintf(b, sizeof(b),
            "[shader(\"closesthit\")] void CH_%u(inout Pay p, BuiltInTriangleIntersectionAttributes a)"
            " { p.c = Shade(%uu, PrimitiveIndex(), RayTCurrent(), a.barycentrics); }\n"
            "[shader(\"closesthit\")] void CR_%u(inout RecPay r, BuiltInTriangleIntersectionAttributes a)"
            " { r.key = %uu; r.prim = PrimitiveIndex(); r.t = RayTCurrent();"
            " r.bx = a.barycentrics.x; r.by = a.barycentrics.y; }\n"
            "[shader(\"callable\")] void CL_%u(inout CallData d)"
            " { d.c = Shade(%uu, d.prim, d.t, float2(d.bx, d.by)); }\n",
            k, k, k, k, k, k);
        s += b;
    }
    return s;
}

static ComPtr<IDxcBlob> Compile(const std::string& src, uint32_t M) {
    HMODULE m = LoadLibraryW(L"dxcompiler.dll");
    if (!m) Throw("LoadLibrary(dxcompiler.dll)", HRESULT_FROM_WIN32(GetLastError()));
    auto create = (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance");
    ComPtr<IDxcCompiler3> c;
    HR(create(CLSID_DxcCompiler, IID_PPV_ARGS(&c)), "create IDxcCompiler3");
    std::wstring def = L"MKEY=" + std::to_wstring(M) + L"u";
    std::vector<const wchar_t*> args = { L"-T", L"lib_6_3", L"-D", def.c_str() };
    DxcBuffer buf{ src.data(), src.size(), DXC_CP_UTF8 };
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

// --- D3D12 helpers ----------------------------------------------------------
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
    b.UAV.pResource = nullptr; cl->ResourceBarrier(1, &b);
}
static void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
        D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r; x.Transition.StateBefore = a; x.Transition.StateAfter = b;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &x);
}

struct Gpu {
    ComPtr<ID3D12Device5> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList4> cl;
    ComPtr<ID3D12Fence> f;
    UINT64 fv = 0;
    HANDLE ev = nullptr;
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

static uint32_t HashC(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

static ComPtr<ID3D12Resource> BuildAS(Gpu& g,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in) {
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
    g.flush();
    return as;
}

// The grid: g_res x g_res quads over [-1,1]^2 at z = 0, two triangles each,
// grouped into M geometries so the hit group record IS the material.
struct Scene { ComPtr<ID3D12Resource> vb, blas, tlas, inst; };
static Scene BuildScene(Gpu& g, bool blocked) {
    const uint32_t G = g_res, M = g_M, Q = G * G;
    std::vector<uint32_t> mat(Q), cnt(M, 0);
    for (uint32_t qi = 0; qi < Q; ++qi) {
        mat[qi] = blocked ? (uint32_t)((uint64_t)qi * M / Q) : HashC(qi * 2654435761u + 7u) % M;
        ++cnt[mat[qi]];
    }
    std::vector<uint32_t> first(M, 0);
    for (uint32_t k = 1; k < M; ++k) first[k] = first[k - 1] + cnt[k - 1];
    Scene s;
    const UINT64 vbBytes = (UINT64)Q * 6 * 12;
    s.vb = Buffer(g.dev.Get(), vbBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    float* v = nullptr;
    HR(s.vb->Map(0, nullptr, (void**)&v), "Map vb");
    std::vector<uint32_t> fill = first;
    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float x0 = -1.0f + 2.0f * (qi % G) / G, x1 = -1.0f + 2.0f * (qi % G + 1) / G;
        const float y0 = -1.0f + 2.0f * (qi / G) / G, y1 = -1.0f + 2.0f * (qi / G + 1) / G;
        const float t[18] = { x0, y0, 0, x1, y0, 0, x1, y1, 0,  x0, y0, 0, x1, y1, 0, x0, y1, 0 };
        std::memcpy(v + (size_t)fill[mat[qi]]++ * 18, t, sizeof(t));
    }
    s.vb->Unmap(0, nullptr);

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geo(M);
    for (uint32_t k = 0; k < M; ++k) {
        auto& d = geo[k];
        d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        d.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        d.Triangles.VertexBuffer.StartAddress = s.vb->GetGPUVirtualAddress() + (UINT64)first[k] * 72;
        d.Triangles.VertexBuffer.StrideInBytes = 12;
        d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        d.Triangles.VertexCount = cnt[k] * 6;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bl{};
    bl.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bl.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bl.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bl.NumDescs = M; bl.pGeometryDescs = geo.data();
    s.blas = BuildAS(g, bl);

    s.inst = Buffer(g.dev.Get(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RAYTRACING_INSTANCE_DESC* id = nullptr;
    HR(s.inst->Map(0, nullptr, (void**)&id), "Map inst");
    std::memset(id, 0, sizeof(*id));
    id->Transform[0][0] = id->Transform[1][1] = id->Transform[2][2] = 1.0f;
    id->InstanceMask = 0xFF;
    id->AccelerationStructure = s.blas->GetGPUVirtualAddress();
    s.inst->Unmap(0, nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tl{};
    tl.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tl.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tl.NumDescs = 1; tl.InstanceDescs = s.inst->GetGPUVirtualAddress();
    s.tlas = BuildAS(g, tl);
    return s;
}

// --- the pipeline -----------------------------------------------------------
enum { RG_PLAIN, RG_REC, RG_CLEAR, RG_SCAN, RG_SCATTER, RG_SHADE, RG_N };
static const wchar_t* kRg[RG_N] = { L"RG_Plain", L"RG_Rec", L"RG_Clear", L"RG_Scan",
                                    L"RG_Scatter", L"RG_Shade" };
struct Pipe {
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12StateObject> so;
    ComPtr<ID3D12Resource> table;
    UINT64 offRg[RG_N], offMiss, offHit, offCall;
};

static Pipe BuildPipe(Gpu& g, IDxcBlob* lib) {
    const uint32_t M = g_M;
    Pipe p;
    D3D12_ROOT_PARAMETER rp[6]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 5;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    for (int i = 0; i < 4; ++i) {
        rp[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp[2 + i].Descriptor.ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 6; rd.pParameters = rp;
    ComPtr<ID3DBlob> blob, err;
    HR(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err), "serialize RS");
    HR(g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&p.rs)), "CreateRootSignature");

    std::vector<std::wstring> names;
    names.reserve(4 * M);
    std::vector<D3D12_HIT_GROUP_DESC> hg(2 * M);
    for (uint32_t k = 0; k < M; ++k) {
        names.push_back(L"HG_" + std::to_wstring(k));
        names.push_back(L"CH_" + std::to_wstring(k));
        names.push_back(L"HR_" + std::to_wstring(k));
        names.push_back(L"CR_" + std::to_wstring(k));
    }
    for (uint32_t k = 0; k < M; ++k) {
        hg[k] = {}; hg[k].Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg[k].HitGroupExport = names[4 * k].c_str();
        hg[k].ClosestHitShaderImport = names[4 * k + 1].c_str();
        hg[M + k] = {}; hg[M + k].Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg[M + k].HitGroupExport = names[4 * k + 2].c_str();
        hg[M + k].ClosestHitShaderImport = names[4 * k + 3].c_str();
    }
    D3D12_DXIL_LIBRARY_DESC ld{};
    ld.DXILLibrary = { lib->GetBufferPointer(), lib->GetBufferSize() };
    D3D12_RAYTRACING_SHADER_CONFIG sc{ 32, 8 };
    D3D12_GLOBAL_ROOT_SIGNATURE grs{ p.rs.Get() };
    D3D12_RAYTRACING_PIPELINE_CONFIG pc{ 1 };
    std::vector<D3D12_STATE_SUBOBJECT> sub;
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &ld });
    for (auto& h : hg) sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &h });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });
    D3D12_STATE_OBJECT_DESC sd{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,
                                (UINT)sub.size(), sub.data() };
    HR(g.dev->CreateStateObject(&sd, IID_PPV_ARGS(&p.so)), "CreateStateObject");

    ComPtr<ID3D12StateObjectProperties> props;
    HR(p.so.As(&props), "StateObjectProperties");
    auto A = [](UINT64 x) { return (x + 63) & ~63ull; };
    const UINT64 rec = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;   // 32
    for (int r = 0; r < RG_N; ++r) p.offRg[r] = 64ull * r;
    p.offMiss = 64ull * RG_N;
    p.offHit = A(p.offMiss + 2 * rec);
    p.offCall = A(p.offHit + 2 * M * rec);
    const UINT64 total = A(p.offCall + M * rec);
    p.table = Buffer(g.dev.Get(), total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* t = nullptr;
    HR(p.table->Map(0, nullptr, (void**)&t), "Map table");
    auto put = [&](UINT64 off, const wchar_t* name) {
        void* id = props->GetShaderIdentifier(name);
        if (!id) throw std::runtime_error("no shader identifier");
        std::memcpy(t + off, id, rec);
    };
    for (int r = 0; r < RG_N; ++r) put(p.offRg[r], kRg[r]);
    put(p.offMiss, L"MS_Plain");
    put(p.offMiss + rec, L"MS_Rec");
    for (uint32_t k = 0; k < M; ++k) {
        put(p.offHit + 2 * k * rec, names[4 * k].c_str());
        put(p.offHit + (2 * k + 1) * rec, names[4 * k + 2].c_str());
        put(p.offCall + k * rec, (L"CL_" + std::to_wstring(k)).c_str());
    }
    p.table->Unmap(0, nullptr);
    return p;
}

struct Bufs { ComPtr<ID3D12Resource> outA, outB, outC, hits, count, sorted; };

static void Dispatch(Gpu& g, const Pipe& p, int rg, UINT w, UINT h) {
    // Diagnostic: SERTEST_SKIP=<n> leaves raygen n out, to find a hang.
    static const char* skip = getenv("SERTEST_SKIP");
    if (skip && std::atoi(skip) == rg) return;
    const D3D12_GPU_VIRTUAL_ADDRESS b = p.table->GetGPUVirtualAddress();
    const UINT64 rec = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    D3D12_DISPATCH_RAYS_DESC d{};
    d.RayGenerationShaderRecord = { b + p.offRg[rg], rec };
    d.MissShaderTable = { b + p.offMiss, 2 * rec, rec };
    d.HitGroupTable = { b + p.offHit, 2 * g_M * rec, rec };
    d.CallableShaderTable = { b + p.offCall, g_M * rec, rec };
    d.Width = w; d.Height = h; d.Depth = 1;
    g.cl->DispatchRays(&d);
}

static void Bind(Gpu& g, const Pipe& p, const Scene& s, const Bufs& b, uint32_t mode,
                 uint32_t iters, uint32_t unsorted, ID3D12Resource* out) {
    g.cl->SetPipelineState1(p.so.Get());
    g.cl->SetComputeRootSignature(p.rs.Get());
    const uint32_t c[5] = { g_res, g_res, mode, iters, unsorted };
    g.cl->SetComputeRoot32BitConstants(0, 5, c, 0);
    g.cl->SetComputeRootShaderResourceView(1, s.tlas->GetGPUVirtualAddress());
    g.cl->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
    g.cl->SetComputeRootUnorderedAccessView(3, b.hits->GetGPUVirtualAddress());
    g.cl->SetComputeRootUnorderedAccessView(4, b.count->GetGPUVirtualAddress());
    g.cl->SetComputeRootUnorderedAccessView(5, b.sorted->GetGPUVirtualAddress());
}

// The reordered path, or with unsorted = 1 the two-pass control. Timestamps
// at ts, ts+1 (after the record trace), ts+2 (after the sort), ts+3 (end).
static void Reordered(Gpu& g, const Pipe& p, const Scene& s, const Bufs& b, uint32_t mode,
                      uint32_t iters, uint32_t unsorted, ID3D12Resource* out,
                      ID3D12QueryHeap* qh, UINT ts) {
    const UINT R = g_res;
    Bind(g, p, s, b, mode, iters, unsorted, out);
    if (qh) g.cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, ts);
    Dispatch(g, p, RG_CLEAR, 2 * (g_M + 1), 1); UavAll(g.cl.Get());
    Dispatch(g, p, RG_REC, R, R); UavAll(g.cl.Get());
    if (qh) g.cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, ts + 1);
    if (!unsorted) {
        Dispatch(g, p, RG_SCAN, 1, 1); UavAll(g.cl.Get());
        Dispatch(g, p, RG_SCATTER, R, R); UavAll(g.cl.Get());
    }
    if (qh) g.cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, ts + 2);
    Dispatch(g, p, RG_SHADE, R, R); UavAll(g.cl.Get());
    if (qh) g.cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, ts + 3);
}
static void Plain(Gpu& g, const Pipe& p, const Scene& s, const Bufs& b, uint32_t mode,
                  uint32_t iters, ID3D12Resource* out, ID3D12QueryHeap* qh, UINT ts) {
    Bind(g, p, s, b, mode, iters, 0, out);
    if (qh) g.cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, ts);
    Dispatch(g, p, RG_PLAIN, g_res, g_res); UavAll(g.cl.Get());
    if (qh) g.cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, ts + 1);
}

template <class T>
static std::vector<T> Readback(Gpu& g, ID3D12Resource* r, size_t n) {
    const UINT64 bytes = n * sizeof(T);
    auto rb = Buffer(g.dev.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    Transition(g.cl.Get(), r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.cl->CopyBufferRegion(rb.Get(), 0, r, 0, bytes);
    Transition(g.cl.Get(), r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.flush();
    std::vector<T> v(n);
    void* m = nullptr;
    HR(rb->Map(0, nullptr, &m), "Map readback");
    std::memcpy(v.data(), m, bytes);
    rb->Unmap(0, nullptr);
    return v;
}

struct F4 { float x, y, z, w; };
struct HitRec { uint32_t key, prim; float t, bx, by; uint32_t p0, p1, p2; };

// Correctness: reordered and two-pass must draw exactly what plain draws, and
// the sort must be a permutation with nondecreasing keys.
static bool Check(Gpu& g, const Pipe& p, const Scene& s, const Bufs& b, uint32_t mode,
                  uint32_t iters, std::string& why) {
    const size_t N = (size_t)g_res * g_res;
    const bool v = getenv("SERTEST_VERBOSE") != nullptr;
    Plain(g, p, s, b, mode, iters, b.outA.Get(), nullptr, 0);
    g.flush(); if (v) std::printf("  plain ok\n");
    Reordered(g, p, s, b, mode, iters, 1, b.outC.Get(), nullptr, 0);
    g.flush(); if (v) std::printf("  two-pass ok\n");
    Reordered(g, p, s, b, mode, iters, 0, b.outB.Get(), nullptr, 0);
    g.flush(); if (v) std::printf("  reordered ok\n");
    auto A = Readback<F4>(g, b.outA.Get(), N);
    auto B = Readback<F4>(g, b.outB.Get(), N);
    auto C = Readback<F4>(g, b.outC.Get(), N);
    auto H = Readback<HitRec>(g, b.hits.Get(), N);
    auto S = Readback<uint32_t>(g, b.sorted.Get(), N);
    size_t mb = 0, mc = 0, hits = 0;
    for (size_t i = 0; i < N; ++i) {
        if (std::memcmp(&A[i], &B[i], 16)) ++mb;
        if (std::memcmp(&A[i], &C[i], 16)) ++mc;
        if (A[i].w >= 0) ++hits;
    }
    std::vector<uint8_t> seen(N, 0);
    size_t bad = 0;
    for (size_t j = 0; j < N; ++j) {
        if (S[j] >= N || seen[S[j]]++) { ++bad; continue; }
        if (j && S[j - 1] < N && H[S[j - 1]].key > H[S[j]].key) ++bad;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%zu of %zu rays hit; reordered differs on %zu, two-pass on %zu; sort errors %zu",
                  hits, N, mb, mc, bad);
    why = buf;
    return mb == 0 && mc == 0 && bad == 0 && hits > N / 2;
}

static double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { if (i + 1 >= argc) throw std::runtime_error("missing value"); return std::string(argv[++i]); };
        if (a == "--m") g_M = (uint32_t)std::stoul(next());
        else if (a == "--res") g_res = (uint32_t)std::stoul(next());
        else if (a == "--iters") g_iters = std::stoi(next());
        else if (a == "--k") {
            g_ks.clear();
            std::string s = next();
            for (size_t pos = 0; pos < s.size();) {
                size_t c = s.find(',', pos);
                g_ks.push_back((uint32_t)std::stoul(s.substr(pos, c - pos)));
                if (c == std::string::npos) break;
                pos = c + 1;
            }
        } else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }
    try {
        ComPtr<IDXGIFactory6> fac;
        HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> ad;
        HR(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&ad)), "EnumAdapterByGpuPreference");
        DXGI_ADAPTER_DESC1 desc{}; ad->GetDesc1(&desc);
        Gpu g;
        HR(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev)), "D3D12CreateDevice");
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
        g.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
        std::printf("adapter: %ls, raytracing tier %d.%d\n", desc.Description,
                    o5.RaytracingTier / 10, o5.RaytracingTier % 10);
        std::printf("%u materials, %ux%u rays, a %ux%u quad grid (%u triangles), median of %d runs\n\n",
                    g_M, g_res, g_res, g_res, g_res, 2 * g_res * g_res, g_iters);
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HR(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.q)), "queue");
        HR(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.al)), "alloc");
        HR(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.al.Get(), nullptr,
            IID_PPV_ARGS(&g.cl)), "list");
        HR(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.f)), "fence");
        g.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        UINT64 freq = 0; HR(g.q->GetTimestampFrequency(&freq), "timestamp frequency");

        auto lib = Compile(MakeSource(g_M), g_M);
        Pipe p = BuildPipe(g, lib.Get());
        const size_t N = (size_t)g_res * g_res;
        const auto UA = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Bufs b;
        b.outA = Buffer(g.dev.Get(), N * 16, D3D12_HEAP_TYPE_DEFAULT, UA, true);
        b.outB = Buffer(g.dev.Get(), N * 16, D3D12_HEAP_TYPE_DEFAULT, UA, true);
        b.outC = Buffer(g.dev.Get(), N * 16, D3D12_HEAP_TYPE_DEFAULT, UA, true);
        b.hits = Buffer(g.dev.Get(), N * 32, D3D12_HEAP_TYPE_DEFAULT, UA, true);
        b.count = Buffer(g.dev.Get(), 8ull * (g_M + 1), D3D12_HEAP_TYPE_DEFAULT, UA, true);
        b.sorted = Buffer(g.dev.Get(), N * 4, D3D12_HEAP_TYPE_DEFAULT, UA, true);

        const UINT kTs = 10;
        D3D12_QUERY_HEAP_DESC qhd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTs, 0 };
        ComPtr<ID3D12QueryHeap> qh;
        HR(g.dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)), "CreateQueryHeap");
        auto tsRb = Buffer(g.dev.Get(), 8 * kTs, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

        std::printf("%-8s %-10s %6s | %9s | %9s = %7s + %6s + %7s | %9s | %s\n",
                    "layout", "rays", "cost", "plain ms", "reorder", "trace", "sort", "shade",
                    "two-pass", "reorder vs plain");
        bool allOk = true;
        for (int blocked = 0; blocked < 2; ++blocked) {
            Scene s = BuildScene(g, blocked != 0);
            for (uint32_t mode = 0; mode < 2; ++mode) {
                for (uint32_t k : g_ks) {
                    std::string why;
                    const bool ok = Check(g, p, s, b, mode, k, why);
                    if (!ok) allOk = false;
                    std::vector<double> tp, tr, tt, tsort, tsh, t2;
                    for (int it = 0; it < g_iters + 2; ++it) {
                        // Alternate the order so clock drift cannot favour one.
                        if (it & 1) {
                            Reordered(g, p, s, b, mode, k, 0, b.outB.Get(), qh.Get(), 2);
                            Plain(g, p, s, b, mode, k, b.outA.Get(), qh.Get(), 0);
                        } else {
                            Plain(g, p, s, b, mode, k, b.outA.Get(), qh.Get(), 0);
                            Reordered(g, p, s, b, mode, k, 0, b.outB.Get(), qh.Get(), 2);
                        }
                        Reordered(g, p, s, b, mode, k, 1, b.outC.Get(), qh.Get(), 6);
                        g.cl->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, kTs, tsRb.Get(), 0);
                        g.flush();
                        uint64_t* q = nullptr;
                        HR(tsRb->Map(0, nullptr, (void**)&q), "Map timestamps");
                        auto ms = [&](int a, int z) { return 1000.0 * (double)(q[z] - q[a]) / freq; };
                        if (it >= 2) {   // two warm-up runs
                            tp.push_back(ms(0, 1));
                            tr.push_back(ms(2, 5)); tt.push_back(ms(2, 3));
                            tsort.push_back(ms(3, 4)); tsh.push_back(ms(4, 5));
                            t2.push_back(ms(6, 9));
                        }
                        tsRb->Unmap(0, nullptr);
                    }
                    const double P = Median(tp), R = Median(tr);
                    char verdict[64];
                    std::snprintf(verdict, sizeof(verdict), "%s %.2fx",
                                  R < P ? "FASTER" : "slower", R < P ? P / R : R / P);
                    std::printf("%-8s %-10s %6u | %9.2f | %9.2f = %7.2f + %6.2f + %7.2f | %9.2f | %s%s\n",
                                blocked ? "blocked" : "random", mode ? "scattered" : "coherent", k,
                                P, R, Median(tt), Median(tsort), Median(tsh), Median(t2), verdict,
                                ok ? "" : "   WRONG ANSWER");
                    if (!ok) std::printf("         %s\n", why.c_str());
                    else if (k == g_ks.front()) std::printf("         check: %s\n", why.c_str());
                }
            }
        }
        std::printf("\n%s\n", allOk ? "every configuration drew the same image three ways"
                                    : "SOME CONFIGURATIONS DREW A DIFFERENT IMAGE, their timings mean nothing");
        return allOk ? 0 : 1;
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
