// Phase 4 probe - the three non-shader Tier 1.1 features, on WARP and hardware.
//
// This is NOT a pass/fail test. It is an instrument. Its job is to establish
// ground truth on WARP, which implements Tier 1.1 correctly in software, and to
// record exactly HOW each feature fails on the GTX 1070, which reports Tier 1.0.
// The shim has to reproduce the first and intercept the second, so both halves
// are the deliverable.
//
// Build:  build_phase4.bat   ->  tier11probe.exe
// Run:    tier11probe.exe            both adapters, all probes
//         tier11probe.exe warp       one adapter
//         tier11probe.exe hw
//
// Probes, each self-checking on a single adapter rather than diffed across two,
// so an adapter that cannot run a probe still reports something useful:
//
//   caps        Tier, shader model, and whether ID3D12Device7 exists at all.
//   indirect    ExecuteIndirect + DISPATCH_RAYS against a direct DispatchRays
//               baseline in the same process. Equal buffers means the indirect
//               path did the same work.
//   addto       AddToStateObject: add a second miss shader to an existing state
//               object, dispatch with it, and check the miss value changed.
//   rayflags    RAY_FLAG_SKIP_TRIANGLES / SKIP_PROCEDURAL_PRIMITIVES on a
//               triangle-only scene. SKIP_TRIANGLES must drop the hit count to
//               zero; SKIP_PROCEDURAL_PRIMITIVES must leave it unchanged. A
//               Tier 1.0 driver that silently ignores the flag is the dangerous
//               outcome, and this is what detects it.
//
// Scene and helpers are lifted from phase2/raytest.cpp deliberately, so this
// file stays self-contained and that verified Phase 2 artifact is not touched.

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
#include <stdexcept>
#include <functional>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// --- Agility SDK opt-in. D3D12Core.dll must live in .\D3D12\ next to the exe.
#if defined(D3D12_SDK_VERSION) && !defined(DXRTEST_AGILITY_VERSION)
  #define DXRTEST_AGILITY_VERSION D3D12_SDK_VERSION
#endif
#ifndef DXRTEST_AGILITY_VERSION
  #define DXRTEST_AGILITY_VERSION 619u
#endif
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = DXRTEST_AGILITY_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

// Same ortho grid as Phase 2, so hit counts are directly comparable with it.
static const uint32_t kWidth  = 256;
static const uint32_t kHeight = 256;
static const float kHalfExtent = 1.5f;
static const float kCamZ = 2.0f;
static const float kTMin = 0.0f;
static const float kTMax = 10.0f;

#pragma pack(push, 1)
struct Result { float t; float bx; float by; uint32_t hit; };
struct SceneCB {
    uint32_t width; uint32_t height;
    float halfExtent; float camZ; float tMin; float tMax;
    float pad0; float pad1;
};
#pragma pack(pop)

enum class Adapter { Warp, Hardware };

// -debug turns on the D3D12 debug layer and drains its InfoQueue after every
// probe. An E_INVALIDARG from a state object call is almost never diagnosable
// without it: the layer names the actual rule that was broken.
static bool g_debug = false;
static bool g_timing = false;
static bool g_pipeline = false;
static bool g_dxgiQueue = false;
static bool g_hookTest = false;
static bool g_queueVTable = false;
static bool g_gfxSplit = false;
static bool g_batchSplit = false;
// Put the instance descriptions in GPU-only memory, as an engine that
// builds them in a compute pass would. The shim can then no longer just
// map them, and has to copy them out and read them after a submission.
static bool g_gpuInst = false;
static int g_filler = 4;
static int g_frames = 60;
static uint32_t g_rayCount = 1u << 20;   // 1M rays, enough that Pascal does real work
static int g_iters = 20;

static void Throw(const char* what, HRESULT hr) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s (hr=0x%08lx)", what, (unsigned long)hr);
    throw std::runtime_error(buf);
}
static void HR(HRESULT hr, const char* what) { if (FAILED(hr)) Throw(what, hr); }

// Report an HRESULT without throwing: probes record failures, they do not abort.
static const char* HrName(HRESULT hr) {
    switch (hr) {
    case S_OK:                  return "S_OK";
    case E_INVALIDARG:          return "E_INVALIDARG";
    case E_NOINTERFACE:         return "E_NOINTERFACE";
    case E_NOTIMPL:             return "E_NOTIMPL";
    case E_OUTOFMEMORY:         return "E_OUTOFMEMORY";
    case E_FAIL:                return "E_FAIL";
    case DXGI_ERROR_UNSUPPORTED:return "DXGI_ERROR_UNSUPPORTED";
    default:                    return "";
    }
}
static std::string HrStr(HRESULT hr) {
    char b[64];
    const char* n = HrName(hr);
    if (n[0]) std::snprintf(b, sizeof(b), "0x%08lx %s", (unsigned long)hr, n);
    else      std::snprintf(b, sizeof(b), "0x%08lx", (unsigned long)hr);
    return b;
}

// --- shaders ---------------------------------------------------------------
//
// One library serves every probe. FLAGS is injected by the compiler so the same
// source can be built with and without the Tier 1.1 ray flags. MISSVAL lets the
// AddToStateObject probe produce a distinguishable second miss shader.

static const char* kLibHLSL = R"HLSL(
#ifndef FLAGS
#define FLAGS RAY_FLAG_FORCE_OPAQUE
#endif
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);
struct Payload { float t; float2 bary; uint hit; };

[shader("raygeneration")]
void RayGen() {
    uint2 idx = DispatchRaysIndex().xy;
    float u = ((idx.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((idx.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    Payload p; p.t = 0; p.bary = float2(0, 0); p.hit = 0;
    TraceRay(scene, FLAGS, 0xFF, 0, 0, 0, ray, p);

    Result r; r.t = p.t; r.bx = p.bary.x; r.by = p.bary.y; r.hit = p.hit;
    outBuf[idx.y * width + idx.x] = r;
}
[shader("closesthit")]
void ClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
    p.t = RayTCurrent(); p.bary = attr.barycentrics; p.hit = 1;
}
[shader("miss")]
void Miss(inout Payload p) { p.hit = 0; }

// --- procedural primitive, so SKIP_PROCEDURAL_PRIMITIVES has something to skip.
// The box must match kAabb in the C++ side.
struct ProcAttr { float unused; };
static const float3 kBoxMin = float3(-1.45, -0.40, -1.10);
static const float3 kBoxMax = float3(-1.05,  0.40, -0.90);

[shader("intersection")]
void Isect() {
    // This harness only ever casts rays along -Z, so a general slab test is not
    // needed. Testing XY containment and solving for the front face in z keeps
    // the result independent of how conservatively a given implementation calls
    // the intersection shader, which matters when comparing WARP to hardware.
    float3 o = ObjectRayOrigin();
    float3 d = ObjectRayDirection();
    if (o.x < kBoxMin.x || o.x > kBoxMax.x) return;
    if (o.y < kBoxMin.y || o.y > kBoxMax.y) return;
    float t = (kBoxMax.z - o.z) / d.z;
    if (t < RayTMin() || t > RayTCurrent()) return;
    ProcAttr a; a.unused = 0;
    ReportHit(t, 0, a);
}
[shader("closesthit")]
void ClosestHitProc(inout Payload p, ProcAttr a) {
    p.t = RayTCurrent(); p.bary = float2(0, 0); p.hit = 2;
}
)HLSL";

// --- timing library --------------------------------------------------------
//
// Used only by the -time mode, to price the two candidate strategies for
// indirect DispatchRays. See docs/phase4-indirect-design.md.
//
// Rays are indexed linearly rather than as a 2D grid, which matches how Unreal
// actually dispatches: a compaction pass produces a 1D list of surviving rays
// and the dispatch covers that count.
//
// BOUNDED=1 adds exactly the work strategy S2 costs every thread: one buffer
// load, one compare, and an early return for threads past the real ray count.
static const char* kTimingHLSL = R"HLSL(
#ifndef BOUNDED
#define BOUNDED 0
#endif
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer dimsBuf : register(t1);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);
struct Payload { float t; float2 bary; uint hit; };

[shader("raygeneration")]
void RayGen() {
    uint idx = DispatchRaysIndex().x;
#if BOUNDED
    uint realCount = dimsBuf.Load(0);
    if (idx >= realCount) return;
#endif
    uint px = idx % width;
    uint py = idx / width;
    float u = ((px + 0.5) / width)  * 2.0 - 1.0;
    float v = ((py + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    Payload p; p.t = 0; p.bary = float2(0, 0); p.hit = 0;
    TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, ray, p);

    Result r; r.t = p.t; r.bx = p.bary.x; r.by = p.bary.y; r.hit = p.hit;
    outBuf[idx] = r;
}
[shader("closesthit")]
void ClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
    p.t = RayTCurrent(); p.bary = attr.barycentrics; p.hit = 1;
}
[shader("miss")]
void Miss(inout Payload p) { p.hit = 0; }

struct ProcAttr { float unused; };
static const float3 kBoxMin = float3(-1.45, -0.40, -1.10);
static const float3 kBoxMax = float3(-1.05,  0.40, -0.90);
[shader("intersection")]
void Isect() {
    float3 o = ObjectRayOrigin();
    float3 d = ObjectRayDirection();
    if (o.x < kBoxMin.x || o.x > kBoxMax.x) return;
    if (o.y < kBoxMin.y || o.y > kBoxMax.y) return;
    float t = (kBoxMax.z - o.z) / d.z;
    if (t < RayTMin() || t > RayTCurrent()) return;
    ProcAttr a; a.unused = 0;
    ReportHit(t, 0, a);
}
[shader("closesthit")]
void ClosestHitProc(inout Payload p, ProcAttr a) {
    p.t = RayTCurrent(); p.bary = float2(0, 0); p.hit = 2;
}
)HLSL";

// The library added by AddToStateObject. Same payload layout, one new export.
// Writing 7 rather than 0 on a miss is what proves the added shader actually ran.
static const char* kAddLibHLSL = R"HLSL(
struct Payload { float t; float2 bary; uint hit; };
[shader("miss")]
void Miss2(inout Payload p) { p.t = 0; p.bary = float2(0, 0); p.hit = 7; }
)HLSL";

// --- DXC -------------------------------------------------------------------
struct Dxc {
    ComPtr<IDxcCompiler3> compiler;
    void init() {
        HMODULE m = LoadLibraryW(L"dxcompiler.dll");
        if (!m) Throw("LoadLibrary(dxcompiler.dll)", HRESULT_FROM_WIN32(GetLastError()));
        auto create = (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance");
        if (!create) Throw("GetProcAddress(DxcCreateInstance)", E_FAIL);
        HR(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "create IDxcCompiler3");
    }
    // Returns null on failure and fills `err`, so a probe can record that DXC
    // itself refused a construct rather than aborting the whole run.
    ComPtr<IDxcBlob> tryCompile(const char* src, const wchar_t* target,
                                const std::vector<std::wstring>& defines,
                                std::string& err, const wchar_t* entry = nullptr) {
        DxcBuffer buf{ src, std::strlen(src), DXC_CP_UTF8 };
        std::vector<const wchar_t*> args = { L"-T", target };
        if (entry) { args.push_back(L"-E"); args.push_back(entry); }
        for (auto& d : defines) { args.push_back(L"-D"); args.push_back(d.c_str()); }
        ComPtr<IDxcResult> res;
        HRESULT hr = compiler->Compile(&buf, args.data(), (UINT32)args.size(),
                                       nullptr, IID_PPV_ARGS(&res));
        if (FAILED(hr)) { err = "IDxcCompiler3::Compile " + HrStr(hr); return nullptr; }
        HRESULT status = E_FAIL; res->GetStatus(&status);
        if (FAILED(status)) {
            ComPtr<IDxcBlobUtf8> e;
            res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&e), nullptr);
            err = (e && e->GetStringLength()) ? e->GetStringPointer() : "(no log)";
            return nullptr;
        }
        ComPtr<IDxcBlob> obj;
        if (FAILED(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr))) {
            err = "no DXIL object"; return nullptr;
        }
        return obj;
    }
};

// --- D3D12 helpers ---------------------------------------------------------
static ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* dev, UINT64 size,
        D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
        nullptr, IID_PPV_ARGS(&r)), "CreateCommittedResource");
    return r;
}
static void UavBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r; cl->ResourceBarrier(1, &b);
}
static void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
        D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
}

struct Gpu {
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceVal = 0;
    HANDLE evt = nullptr;

    void init(ID3D12Device5* dev) {
        device = dev;
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HR(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&alloc)), "CreateCommandAllocator");
        HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
            nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
        HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    void flush() {
        HR(list->Close(), "cmdlist Close");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        HR(queue->Signal(fence.Get(), ++fenceVal), "queue Signal");
        if (fence->GetCompletedValue() < fenceVal) {
            HR(fence->SetEventOnCompletion(fenceVal, evt), "SetEventOnCompletion");
            WaitForSingleObject(evt, INFINITE);
        }
        HR(alloc->Reset(), "allocator Reset");
        HR(list->Reset(alloc.Get(), nullptr), "cmdlist Reset");
    }
};

struct Scene {
    // Triangles and procedural AABBs need SEPARATE bottom-level structures: a
    // BLAS carries one geometry type. Putting both in one BLAS makes the NVIDIA
    // driver return a zero-sized prebuild info, which then shows up as a
    // confusing E_INVALIDARG from CreateCommittedResource. WARP tolerates it,
    // which is exactly the sort of difference this probe exists to catch.
    ComPtr<ID3D12Resource> vb, aabb, blasTri, blasAabb, tlas;
};

// Procedural primitive bounds. Must match kBoxMin/kBoxMax in the HLSL above.
// Chosen to sit entirely left of the triangle so the two never overlap in XY:
// the triangle's left edge spans x = -0.7 .. -0.3 over y = -0.4 .. 0.4, and this
// box ends at x = -1.05. That keeps the hit counts cleanly separable.
static const D3D12_RAYTRACING_AABB kAabb = {
    -1.45f, -0.40f, -1.10f,
    -1.05f,  0.40f, -0.90f
};

static ComPtr<ID3D12Resource> BuildAS(Gpu& g,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    g.device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    // A driver that dislikes the inputs reports zero sizes here rather than an
    // error, and the confusion surfaces two calls later as a failed buffer
    // creation. Say what actually happened instead.
    if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
        throw std::runtime_error(
            "GetRaytracingAccelerationStructurePrebuildInfo returned zero sizes; "
            "this driver rejected the acceleration structure inputs");
    auto scratch = CreateBuffer(g.device.Get(), info.ScratchDataSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto result = CreateBuffer(g.device.Get(), info.ResultDataMaxSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.Inputs = inputs;
    desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    desc.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    g.list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    UavBarrier(g.list.Get(), result.Get());
    g.flush();
    return result;
}

static Scene BuildScene(Gpu& g) {
    Scene s;
    const float verts[9] = {
        0.0f,  1.0f, 0.0f,
        1.0f, -1.0f, 0.0f,
       -1.0f, -1.0f, 0.0f,
    };
    s.vb = CreateBuffer(g.device.Get(), sizeof(verts), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(s.vb->Map(0, &none, &p), "map vb"); std::memcpy(p, verts, sizeof(verts));
    s.vb->Unmap(0, nullptr);

    s.aabb = CreateBuffer(g.device.Get(), sizeof(kAabb), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    HR(s.aabb->Map(0, &none, &p), "map aabb"); std::memcpy(p, &kAabb, sizeof(kAabb));
    s.aabb->Unmap(0, nullptr);

    D3D12_RAYTRACING_GEOMETRY_DESC tri{};
    tri.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    tri.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    tri.Triangles.VertexBuffer.StartAddress = s.vb->GetGPUVirtualAddress();
    tri.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
    tri.Triangles.VertexCount = 3;
    tri.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    tri.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bi{};
    bi.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bi.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bi.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bi.NumDescs = 1; bi.pGeometryDescs = &tri;
    s.blasTri = BuildAS(g, bi);

    D3D12_RAYTRACING_GEOMETRY_DESC box{};
    box.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    box.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    box.AABBs.AABBCount = 1;
    box.AABBs.AABBs.StartAddress = s.aabb->GetGPUVirtualAddress();
    box.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
    bi.pGeometryDescs = &box;
    s.blasAabb = BuildAS(g, bi);

    // Two instances. InstanceContributionToHitGroupIndex is what selects the hit
    // group record, so instance 0 lands on the triangle hit group and instance 1
    // on the procedural one. Doing it per instance rather than per geometry also
    // means TraceRay can keep its geometry multiplier at 0.
    D3D12_RAYTRACING_INSTANCE_DESC inst[2]{};
    for (int i = 0; i < 2; ++i) {
        inst[i].Transform[0][0] = inst[i].Transform[1][1] = inst[i].Transform[2][2] = 1.0f;
        inst[i].InstanceMask = 0xFF;
        inst[i].InstanceContributionToHitGroupIndex = (UINT)i;
    }
    inst[0].AccelerationStructure = s.blasTri->GetGPUVirtualAddress();
    inst[1].AccelerationStructure = s.blasAabb->GetGPUVirtualAddress();

    auto instUpload = CreateBuffer(g.device.Get(), sizeof(inst), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    HR(instUpload->Map(0, &none, &p), "map inst"); std::memcpy(p, inst, sizeof(inst));
    instUpload->Unmap(0, nullptr);

    // By default the descriptions stay in the upload buffer, which is what
    // every sample does and what the CPU can read directly. With -gpuinst they
    // are moved to a DEFAULT heap first, so the only way to see them is a copy
    // out after the GPU has run. DXR wants the buffer in
    // NON_PIXEL_SHADER_RESOURCE at the build, so it is left in that state.
    ComPtr<ID3D12Resource> instBuf = instUpload;
    if (g_gpuInst) {
        instBuf = CreateBuffer(g.device.Get(), sizeof(inst), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyBufferRegion(instBuf.Get(), 0, instUpload.Get(), 0, sizeof(inst));
        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = instBuf.Get();
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        g.list->ResourceBarrier(1, &tb);
        std::printf("   instance descriptions placed in GPU-only memory (-gpuinst)\n");
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
    ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    ti.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    ti.NumDescs = 2; ti.InstanceDescs = instBuf->GetGPUVirtualAddress();
    s.tlas = BuildAS(g, ti);
    return s;
}

static ComPtr<ID3D12RootSignature> MakeRootSig(ID3D12Device* dev) {
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 3; rd.pParameters = params;
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) Throw("D3D12SerializeRootSignature", hr);
    ComPtr<ID3D12RootSignature> rs;
    HR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&rs)), "CreateRootSignature");
    return rs;
}

static void BindRoots(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs,
        D3D12_GPU_VIRTUAL_ADDRESS cb, D3D12_GPU_VIRTUAL_ADDRESS tlas,
        D3D12_GPU_VIRTUAL_ADDRESS out) {
    cl->SetComputeRootSignature(rs);
    cl->SetComputeRootConstantBufferView(0, cb);
    cl->SetComputeRootShaderResourceView(1, tlas);
    cl->SetComputeRootUnorderedAccessView(2, out);
}

// Timing mode adds t1, the buffer holding the real ray count that a bounded
// raygen reads. Kept separate so the feature probes above stay untouched.
static ComPtr<ID3D12RootSignature> MakeRootSigTiming(ID3D12Device* dev) {
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 4; rd.pParameters = params;
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) Throw("D3D12SerializeRootSignature(timing)", hr);
    ComPtr<ID3D12RootSignature> rs;
    HR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&rs)), "CreateRootSignature(timing)");
    return rs;
}

static void BindRootsTiming(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs,
        D3D12_GPU_VIRTUAL_ADDRESS cb, D3D12_GPU_VIRTUAL_ADDRESS tlas,
        D3D12_GPU_VIRTUAL_ADDRESS out, D3D12_GPU_VIRTUAL_ADDRESS dims) {
    cl->SetComputeRootSignature(rs);
    cl->SetComputeRootConstantBufferView(0, cb);
    cl->SetComputeRootShaderResourceView(1, tlas);
    cl->SetComputeRootUnorderedAccessView(2, out);
    cl->SetComputeRootShaderResourceView(3, dims);
}

// One shader table holding `n` records, each a bare shader identifier padded to
// the record alignment. Record stride is therefore kRecStride, not idSize.
static const UINT kIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;          // 32
static const UINT kRecStride = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT; // 32

static ComPtr<ID3D12Resource> MakeSBT(ID3D12Device* dev,
                                      const void* const* idents, UINT n) {
    UINT64 size = (UINT64)kRecStride * n;
    size = (size + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT - 1)
         & ~(UINT64)(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT - 1);
    auto buf = CreateBuffer(dev, size, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(buf->Map(0, &none, (void**)&p), "map SBT");
    std::memset(p, 0, (size_t)size);
    for (UINT i = 0; i < n; ++i)
        std::memcpy(p + (size_t)i * kRecStride, idents[i], kIdSize);
    buf->Unmap(0, nullptr);
    return buf;
}
static ComPtr<ID3D12Resource> MakeSBT(ID3D12Device* dev, const void* ident) {
    return MakeSBT(dev, &ident, 1);
}

// --- per-adapter context ---------------------------------------------------
struct Ctx {
    Adapter which;
    const char* name;
    Gpu g;
    Dxc dxc;
    Scene scene;
    ComPtr<ID3D12Resource> cb, out, readback;
    ComPtr<ID3D12RootSignature> rs;
    UINT64 outSize = 0;
    D3D12_RAYTRACING_TIER tier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    D3D_SHADER_MODEL sm = D3D_SHADER_MODEL_6_0;
    bool hasDevice7 = false;
    ComPtr<ID3D12InfoQueue> info;

    // Print and clear whatever the debug layer has queued. No-op without -debug.
    void drain(const char* where) {
        if (!info) return;
        UINT64 n = info->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            if (FAILED(info->GetMessage(i, nullptr, &len)) || !len) continue;
            std::vector<char> raw(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(raw.data());
            if (FAILED(info->GetMessage(i, m, &len))) continue;
            if (m->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;  // skip info/message
            std::printf("   [debug-layer/%s] %.*s\n", where,
                        (int)m->DescriptionByteLength, m->pDescription);
        }
        info->ClearStoredMessages();
    }

    // Read the UAV back and bucket the per-ray hit codes:
    //   1 triangle closest-hit, 2 procedural closest-hit, 7 the added Miss2,
    //   0 ordinary miss, 99 never written by any shader.
    void readback_counts(uint32_t& tri, uint32_t& proc, uint32_t& tagged,
                         uint32_t& unwritten, uint32_t tag) {
        Transition(g.list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.list->CopyResource(readback.Get(), out.Get());
        Transition(g.list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.flush();
        std::vector<Result> v(kWidth * kHeight);
        void* rp = nullptr; D3D12_RANGE full{ 0, (SIZE_T)outSize };
        HR(readback->Map(0, &full, &rp), "map readback");
        std::memcpy(v.data(), rp, outSize);
        D3D12_RANGE nowrite{ 0, 0 }; readback->Unmap(0, &nowrite);
        tri = proc = tagged = unwritten = 0;
        for (auto& r : v) {
            if (r.hit == 1)   ++tri;
            if (r.hit == 2)   ++proc;
            if (r.hit == tag) ++tagged;
            if (r.hit == 99)  ++unwritten;
        }
    }
    void clear_out() {
        // Fill with a value no shader writes, so a dispatch that never ran is
        // distinguishable from one that ran and wrote zeroes.
        std::vector<Result> z(kWidth * kHeight);
        for (auto& r : z) { r.t = 0; r.bx = 0; r.by = 0; r.hit = 99; }
        auto up = CreateBuffer(g.device.Get(), outSize, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr; D3D12_RANGE none{ 0, 0 };
        HR(up->Map(0, &none, &p), "map clear");
        std::memcpy(p, z.data(), outSize);
        up->Unmap(0, nullptr);
        Transition(g.list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyResource(out.Get(), up.Get());
        Transition(g.list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.flush();  // upload buffer must outlive the copy
    }
};

// Build a raytracing state object from one or two libraries.
// `allowAdditions` sets the config flag AddToStateObject requires.
static HRESULT MakeStateObject(Ctx& c, IDxcBlob* lib, bool allowAdditions,
                               ComPtr<ID3D12StateObject>& so,
                               ID3D12RootSignature* rootSig = nullptr) {
    std::vector<D3D12_STATE_SUBOBJECT> subs;

    D3D12_STATE_OBJECT_CONFIG cfg{};
    cfg.Flags = allowAdditions ? D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS
                               : D3D12_STATE_OBJECT_FLAG_NONE;
    if (allowAdditions) subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &cfg });

    D3D12_DXIL_LIBRARY_DESC libDesc{};
    libDesc.DXILLibrary.pShaderBytecode = lib->GetBufferPointer();
    libDesc.DXILLibrary.BytecodeLength = lib->GetBufferSize();
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc });

    D3D12_HIT_GROUP_DESC hg{};
    hg.HitGroupExport = L"HitGroup";
    hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg.ClosestHitShaderImport = L"ClosestHit";
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg });

    D3D12_HIT_GROUP_DESC hgProc{};
    hgProc.HitGroupExport = L"HitGroupProc";
    hgProc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
    hgProc.ClosestHitShaderImport = L"ClosestHitProc";
    hgProc.IntersectionShaderImport = L"Isect";
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgProc });

    D3D12_RAYTRACING_SHADER_CONFIG sc{};
    sc.MaxPayloadSizeInBytes = 16;
    sc.MaxAttributeSizeInBytes = 8;
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });

    ID3D12RootSignature* rsPtr = rootSig ? rootSig : c.rs.Get();
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &rsPtr });

    D3D12_RAYTRACING_PIPELINE_CONFIG pc{}; pc.MaxTraceRecursionDepth = 1;
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });

    D3D12_STATE_OBJECT_DESC desc{};
    desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    desc.NumSubobjects = (UINT)subs.size();
    desc.pSubobjects = subs.data();
    return c.g.device->CreateStateObject(&desc, IID_PPV_ARGS(&so));
}

// Fill a DISPATCH_RAYS_DESC for a 1-record-per-table layout.
// `hitRecords` is 2 here: geometry 0 uses hit group record 0, geometry 1 uses
// record 1, via GeometryContributionToHitGroupIndex.
static D3D12_DISPATCH_RAYS_DESC MakeDispatchDesc(
        ID3D12Resource* rayGen, ID3D12Resource* miss,
        ID3D12Resource* hit, UINT hitRecords) {
    D3D12_DISPATCH_RAYS_DESC dr{};
    dr.RayGenerationShaderRecord.StartAddress = rayGen->GetGPUVirtualAddress();
    dr.RayGenerationShaderRecord.SizeInBytes = kIdSize;
    dr.MissShaderTable.StartAddress = miss->GetGPUVirtualAddress();
    dr.MissShaderTable.SizeInBytes = kRecStride;
    dr.MissShaderTable.StrideInBytes = kRecStride;
    dr.HitGroupTable.StartAddress = hit->GetGPUVirtualAddress();
    dr.HitGroupTable.SizeInBytes = kRecStride * hitRecords;
    dr.HitGroupTable.StrideInBytes = kRecStride;
    dr.Width = kWidth; dr.Height = kHeight; dr.Depth = 1;
    return dr;
}

// ---------------------------------------------------------------------------
// Probe 1: capabilities
// ---------------------------------------------------------------------------
static void ProbeCaps(Ctx& c) {
    std::printf("\n-- caps --\n");
    std::printf("   RaytracingTier          : 0x%x (%s)\n", c.tier,
        c.tier >= D3D12_RAYTRACING_TIER_1_1 ? "1.1" :
        c.tier >= D3D12_RAYTRACING_TIER_1_0 ? "1.0" : "none");
    std::printf("   HighestShaderModel      : 0x%x\n", c.sm);
    std::printf("   ID3D12Device7           : %s\n", c.hasDevice7 ? "present" : "ABSENT");
    // Whether Device7 exists is independent of the raytracing tier. If it is
    // present on Tier 1.0 hardware then AddToStateObject is callable there and
    // must be intercepted, not merely absent.
}

// ---------------------------------------------------------------------------
// Probe 2: indirect DispatchRays
//
// Runs the same dispatch twice on one adapter, once directly and once through
// ExecuteIndirect with a DISPATCH_RAYS command signature, and compares the hit
// counts. This is self-checking, so an adapter that cannot build the command
// signature still tells us exactly where it stops.
// ---------------------------------------------------------------------------
static void ProbeIndirect(Ctx& c, IDxcBlob* lib) {
    std::printf("\n-- indirect DispatchRays --\n");

    ComPtr<ID3D12StateObject> so;
    HRESULT hr = MakeStateObject(c, lib, false, so);
    if (FAILED(hr)) { std::printf("   CreateStateObject        : FAILED %s\n", HrStr(hr).c_str()); return; }
    ComPtr<ID3D12StateObjectProperties> props;
    HR(so.As(&props), "StateObjectProperties");

    auto sbtRay  = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"RayGen"));
    auto sbtMiss = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"Miss"));
    const void* hitIds[2] = { props->GetShaderIdentifier(L"HitGroup"),
                              props->GetShaderIdentifier(L"HitGroupProc") };
    auto sbtHit  = MakeSBT(c.g.device.Get(), hitIds, 2);
    auto dr = MakeDispatchDesc(sbtRay.Get(), sbtMiss.Get(), sbtHit.Get(), 2);

    // --- direct baseline ---
    c.clear_out();
    c.g.list->SetPipelineState1(so.Get());
    BindRoots(c.g.list.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
              c.scene.tlas->GetGPUVirtualAddress(), c.out->GetGPUVirtualAddress());
    c.g.list->DispatchRays(&dr);
    UavBarrier(c.g.list.Get(), c.out.Get());
    c.g.flush();
    uint32_t directTri = 0, directProc = 0, ignore = 0, unwritten = 0;
    c.readback_counts(directTri, directProc, ignore, unwritten, 0);
    std::printf("   direct DispatchRays      : %u tri + %u proc hits, %u unwritten\n", directTri, directProc, unwritten);

    // --- command signature ---
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    D3D12_COMMAND_SIGNATURE_DESC csd{};
    csd.ByteStride = sizeof(D3D12_DISPATCH_RAYS_DESC);
    csd.NumArgumentDescs = 1;
    csd.pArgumentDescs = &arg;
    csd.NodeMask = 0;
    ComPtr<ID3D12CommandSignature> cs;
    // pRootSignature must be null when the signature carries no root arguments.
    hr = c.g.device->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&cs));
    std::printf("   CreateCommandSignature   : %s  (ByteStride=%u)\n",
                SUCCEEDED(hr) ? "OK" : ("FAILED " + HrStr(hr)).c_str(),
                (unsigned)csd.ByteStride);
    if (FAILED(hr)) {
        std::printf("   -> this is the Phase 4 interception point for indirect dispatch\n");
        return;
    }

    // --- indirect ---
    auto argBuf = CreateBuffer(c.g.device.Get(), sizeof(dr), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(argBuf->Map(0, &none, &p), "map arg buffer");
    std::memcpy(p, &dr, sizeof(dr));
    argBuf->Unmap(0, nullptr);

    c.clear_out();
    c.g.list->SetPipelineState1(so.Get());
    BindRoots(c.g.list.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
              c.scene.tlas->GetGPUVirtualAddress(), c.out->GetGPUVirtualAddress());
    c.g.list->ExecuteIndirect(cs.Get(), 1, argBuf.Get(), 0, nullptr, 0);
    UavBarrier(c.g.list.Get(), c.out.Get());
    c.g.flush();
    uint32_t indTri = 0, indProc = 0;
    c.readback_counts(indTri, indProc, ignore, unwritten, 0);
    std::printf("   ExecuteIndirect (CPU args): %u tri + %u proc hits, %u unwritten\n", indTri, indProc, unwritten);
    std::printf("   RESULT                   : %s\n",
        (indTri == directTri && indProc == directProc && unwritten == 0) ? "MATCH" : "DIVERGE");

    // --- the case that actually matters -------------------------------------
    // Unreal never hands ExecuteIndirect a CPU-visible argument buffer. It builds
    // one on the GPU with CopyBufferRegion immediately before the dispatch, so at
    // record time nothing in it is valid yet. Reproduce that shape exactly: a
    // DEFAULT heap buffer filled by a copy recorded just ahead of the dispatch.
    auto gpuArgs = CreateBuffer(c.g.device.Get(), sizeof(dr), D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    c.clear_out();
    c.g.list->CopyBufferRegion(gpuArgs.Get(), 0, argBuf.Get(), 0, sizeof(dr));
    // What a well-behaved app does, and what Unreal does: the argument buffer
    // must be in INDIRECT_ARGUMENT state when ExecuteIndirect reads it.
    Transition(c.g.list.Get(), gpuArgs.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    c.g.list->SetPipelineState1(so.Get());
    BindRoots(c.g.list.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
              c.scene.tlas->GetGPUVirtualAddress(), c.out->GetGPUVirtualAddress());
    c.g.list->ExecuteIndirect(cs.Get(), 1, gpuArgs.Get(), 0, nullptr, 0);
    UavBarrier(c.g.list.Get(), c.out.Get());
    c.g.flush();
    uint32_t gpuTri = 0, gpuProc = 0;
    c.readback_counts(gpuTri, gpuProc, ignore, unwritten, 0);
    const bool gpuOk = (gpuTri == directTri && gpuProc == directProc && unwritten == 0);
    std::printf("   ExecuteIndirect (GPU args): %u tri + %u proc hits, %u unwritten\n",
                gpuTri, gpuProc, unwritten);
    std::printf("   RESULT                   : %s%s\n", gpuOk ? "MATCH" : "DIVERGE",
                gpuOk ? "  <- the shape Unreal uses"
                      : "  <- needs the command list split");
}

// ---------------------------------------------------------------------------
// Probe 3: AddToStateObject
//
// Base object has Miss (writes hit=0). The addition contributes Miss2 (writes
// hit=7). Dispatching the grown object with Miss2 in the miss table proves the
// added shader actually ran, rather than merely that the call returned S_OK.
// ---------------------------------------------------------------------------
static void ProbeAddToStateObject(Ctx& c, IDxcBlob* lib, IDxcBlob* addLib) {
    std::printf("\n-- AddToStateObject --\n");

    if (!c.hasDevice7) { std::printf("   ID3D12Device7            : ABSENT, cannot call\n"); return; }
    ComPtr<ID3D12Device7> dev7;
    HR(c.g.device.As(&dev7), "QI ID3D12Device7");

    ComPtr<ID3D12StateObject> base;
    HRESULT hr = MakeStateObject(c, lib, true, base);
    std::printf("   base SO with ALLOW_STATE_OBJECT_ADDITIONS : %s\n",
                SUCCEEDED(hr) ? "OK" : ("FAILED " + HrStr(hr)).c_str());
    if (FAILED(hr)) {
        std::printf("   -> Tier 1.1 is required for the config flag itself\n");
        return;
    }

    // MEASURED, not assumed: the configs are NOT inherited by the new exports.
    // An addition carrying only the DXIL library fails with E_INVALIDARG, and
    // the debug layer says why:
    //   "Subobject association of type RAYTRACING_SHADER_CONFIG must be defined
    //    for all relevant exports, yet no such subobject exists at all. An
    //    example of an export needing this association is Miss2."
    // and the same for RAYTRACING_PIPELINE_CONFIG. So an addition must repeat
    // both configs. This matters directly for Phase 4: the shim emulates
    // AddToStateObject by rebuilding from cached subobjects, and it has to
    // carry the configs across for every newly added export.
    // Also measured: the ADDITION must repeat the config flag too, not just the
    // object being grown. "In this case the addition is missing the flag."
    std::vector<D3D12_STATE_SUBOBJECT> addSubs;

    D3D12_STATE_OBJECT_CONFIG addCfg{};
    addCfg.Flags = D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS;
    addSubs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &addCfg });

    D3D12_DXIL_LIBRARY_DESC addDesc{};
    addDesc.DXILLibrary.pShaderBytecode = addLib->GetBufferPointer();
    addDesc.DXILLibrary.BytecodeLength = addLib->GetBufferSize();
    addSubs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &addDesc });

    D3D12_RAYTRACING_SHADER_CONFIG addSc{};
    addSc.MaxPayloadSizeInBytes = 16;
    addSc.MaxAttributeSizeInBytes = 8;
    addSubs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &addSc });

    D3D12_RAYTRACING_PIPELINE_CONFIG addPc{}; addPc.MaxTraceRecursionDepth = 1;
    addSubs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &addPc });

    D3D12_STATE_OBJECT_DESC addSo{};
    addSo.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    addSo.NumSubobjects = (UINT)addSubs.size();
    addSo.pSubobjects = addSubs.data();

    ComPtr<ID3D12StateObject> grown;
    hr = dev7->AddToStateObject(&addSo, base.Get(), IID_PPV_ARGS(&grown));
    std::printf("   AddToStateObject         : %s\n",
                SUCCEEDED(hr) ? "OK" : ("FAILED " + HrStr(hr)).c_str());
    c.drain("AddToStateObject");
    if (FAILED(hr)) {
        std::printf("   -> this is the Phase 4 interception point for AddToStateObject\n");
        return;
    }

    ComPtr<ID3D12StateObjectProperties> props;
    HR(grown.As(&props), "grown StateObjectProperties");
    const void* idRay   = props->GetShaderIdentifier(L"RayGen");
    const void* idHit   = props->GetShaderIdentifier(L"HitGroup");
    const void* idMiss2 = props->GetShaderIdentifier(L"Miss2");
    std::printf("   identifiers after growth : RayGen=%s HitGroup=%s Miss2=%s\n",
                idRay ? "ok" : "NULL", idHit ? "ok" : "NULL", idMiss2 ? "ok" : "NULL");
    const void* idHitP = props->GetShaderIdentifier(L"HitGroupProc");
    if (!idRay || !idHit || !idHitP || !idMiss2) return;

    auto sbtRay  = MakeSBT(c.g.device.Get(), idRay);
    auto sbtMiss = MakeSBT(c.g.device.Get(), idMiss2);   // the ADDED miss shader
    const void* hitIds[2] = { idHit, idHitP };
    auto sbtHit  = MakeSBT(c.g.device.Get(), hitIds, 2);
    auto dr = MakeDispatchDesc(sbtRay.Get(), sbtMiss.Get(), sbtHit.Get(), 2);

    c.clear_out();
    c.g.list->SetPipelineState1(grown.Get());
    BindRoots(c.g.list.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
              c.scene.tlas->GetGPUVirtualAddress(), c.out->GetGPUVirtualAddress());
    c.g.list->DispatchRays(&dr);
    UavBarrier(c.g.list.Get(), c.out.Get());
    c.g.flush();
    uint32_t tri = 0, proc = 0, miss2 = 0, unwritten = 0;
    c.readback_counts(tri, proc, miss2, unwritten, 7);
    std::printf("   dispatch with added Miss2: %u tri + %u proc hits, %u took Miss2\n", tri, proc, miss2);
    std::printf("   RESULT                   : %s\n",
        (miss2 > 0 && tri + proc + miss2 == kWidth * kHeight) ? "added shader ran" : "DIVERGE");
}

// ---------------------------------------------------------------------------
// Probe 4: Tier 1.1 ray flags
//
// The scene is triangles only, so the expected answers are unambiguous:
//   no flag                      -> baseline hit count
//   SKIP_TRIANGLES               -> 0 hits
//   SKIP_PROCEDURAL_PRIMITIVES   -> baseline hit count
//
// A Tier 1.0 driver that accepts the shader but ignores SKIP_TRIANGLES would
// report the baseline count in row two. That is the silent-wrong-answer case
// and the whole reason this probe exists.
// ---------------------------------------------------------------------------
static void ProbeRayFlags(Ctx& c) {
    std::printf("\n-- Tier 1.1 ray flags --\n");

    struct Case { const char* label; const wchar_t* define; };
    const Case cases[] = {
        { "none (baseline)",           L"FLAGS=RAY_FLAG_FORCE_OPAQUE" },
        { "SKIP_TRIANGLES",            L"FLAGS=RAY_FLAG_FORCE_OPAQUE|RAY_FLAG_SKIP_TRIANGLES" },
        { "SKIP_PROCEDURAL_PRIMITIVES",L"FLAGS=RAY_FLAG_FORCE_OPAQUE|RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES" },
    };

    for (const auto& cs : cases) {
        std::string err;
        // These flags are Shader Model 6.5, so the library target is lib_6_5.
        auto blob = c.dxc.tryCompile(kLibHLSL, L"lib_6_5", { cs.define }, err);
        if (!blob) {
            std::printf("   %-28s: DXC REFUSED: %s\n", cs.label,
                        err.substr(0, 90).c_str());
            continue;
        }
        ComPtr<ID3D12StateObject> so;
        HRESULT hr = MakeStateObject(c, blob.Get(), false, so);
        if (FAILED(hr)) {
            std::printf("   %-28s: CreateStateObject FAILED %s\n", cs.label, HrStr(hr).c_str());
            continue;
        }
        ComPtr<ID3D12StateObjectProperties> props;
        HR(so.As(&props), "StateObjectProperties");
        auto sbtRay  = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"RayGen"));
        auto sbtMiss = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"Miss"));
        const void* hitIds[2] = { props->GetShaderIdentifier(L"HitGroup"),
                                  props->GetShaderIdentifier(L"HitGroupProc") };
        auto sbtHit  = MakeSBT(c.g.device.Get(), hitIds, 2);
        auto dr = MakeDispatchDesc(sbtRay.Get(), sbtMiss.Get(), sbtHit.Get(), 2);

        c.clear_out();
        c.g.list->SetPipelineState1(so.Get());
        BindRoots(c.g.list.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
                  c.scene.tlas->GetGPUVirtualAddress(), c.out->GetGPUVirtualAddress());
        c.g.list->DispatchRays(&dr);
        UavBarrier(c.g.list.Get(), c.out.Get());
        c.g.flush();
        uint32_t tri = 0, proc = 0, ignore = 0, unwritten = 0;
        c.readback_counts(tri, proc, ignore, unwritten, 0);
        std::printf("   %-28s: %6u triangle + %5u procedural%s\n", cs.label, tri, proc,
                    unwritten ? "  (WARNING: some cells unwritten)" : "");
    }
    std::printf("   expected: row1 both nonzero, row2 0 triangles, row3 0 procedural\n");
}

// ---------------------------------------------------------------------------
// Timing mode: price the two candidate strategies for indirect DispatchRays.
//
// The dimensions of an indirect ray dispatch do not exist until the GPU has run,
// so with identical results required there are only two options:
//
//   S1  split the command list, submit, wait on the CPU, read the dimensions
//       back, then dispatch directly. Always correct, costs a pipeline bubble.
//   S2  over-dispatch a bound G and have the raygen early-out past the real
//       count. No sync at all, costs the wasted thread launches.
//
// Neither cost is knowable by argument, so measure both. Everything here runs
// the same ray workload; only the dispatch strategy changes.
// ---------------------------------------------------------------------------
static double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void ProbeTiming(Ctx& c, uint32_t rayCount, int iters) {
    // The ray grid has to match the ray count, or most rays land outside the
    // scene, miss instantly, and the "baseline" measures nothing. Square it up
    // so the hit fraction matches the feature probes (~22%).
    uint32_t side = 1;
    while ((uint64_t)(side + 1) * (side + 1) <= rayCount) ++side;
    rayCount = side * side;

    std::printf("\n-- timing: indirect DispatchRays strategies --\n");
    std::printf("   %u rays over a %ux%u grid, %d timed iterations each\n",
                rayCount, side, side, iters);

    auto rs = MakeRootSigTiming(c.g.device.Get());

    // Own constant buffer, so width/height describe the timing grid rather than
    // the 256x256 one the feature probes use.
    SceneCB cbData{ side, side, kHalfExtent, kCamZ, kTMin, kTMax, 0, 0 };
    UINT64 cbSize = (sizeof(SceneCB) + 255) & ~255ull;
    auto cb = CreateBuffer(c.g.device.Get(), cbSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    {
        void* p = nullptr; D3D12_RANGE none{ 0, 0 };
        HR(cb->Map(0, &none, &p), "map timing cb");
        std::memcpy(p, &cbData, sizeof(cbData));
        cb->Unmap(0, nullptr);
    }

    std::string err;
    auto libPlain = c.dxc.tryCompile(kTimingHLSL, L"lib_6_3", { L"BOUNDED=0" }, err);
    if (!libPlain) { std::printf("   [!] plain timing shader failed: %s\n", err.c_str()); return; }
    auto libBound = c.dxc.tryCompile(kTimingHLSL, L"lib_6_3", { L"BOUNDED=1" }, err);
    if (!libBound) { std::printf("   [!] bounded timing shader failed: %s\n", err.c_str()); return; }

    ComPtr<ID3D12StateObject> soPlain, soBound;
    HRESULT hr = MakeStateObject(c, libPlain.Get(), false, soPlain, rs.Get());
    if (FAILED(hr)) { std::printf("   [!] plain state object: %s\n", HrStr(hr).c_str()); return; }
    hr = MakeStateObject(c, libBound.Get(), false, soBound, rs.Get());
    if (FAILED(hr)) { std::printf("   [!] bounded state object: %s\n", HrStr(hr).c_str()); return; }

    auto sbtFor = [&](ID3D12StateObject* so, ComPtr<ID3D12Resource>& ray,
                      ComPtr<ID3D12Resource>& miss, ComPtr<ID3D12Resource>& hit) {
        ComPtr<ID3D12StateObjectProperties> props;
        HR(so->QueryInterface(IID_PPV_ARGS(&props)), "props");
        ray  = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"RayGen"));
        miss = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"Miss"));
        const void* ids[2] = { props->GetShaderIdentifier(L"HitGroup"),
                               props->GetShaderIdentifier(L"HitGroupProc") };
        hit = MakeSBT(c.g.device.Get(), ids, 2);
    };
    ComPtr<ID3D12Resource> rayP, missP, hitP, rayB, missB, hitB;
    sbtFor(soPlain.Get(), rayP, missP, hitP);
    sbtFor(soBound.Get(), rayB, missB, hitB);
    auto drPlain = MakeDispatchDesc(rayP.Get(), missP.Get(), hitP.Get(), 2);
    auto drBound = MakeDispatchDesc(rayB.Get(), missB.Get(), hitB.Get(), 2);

    // Output sized for the real ray count. The bounded shader never writes past
    // it, which is the whole point of the early-out.
    const UINT64 outSize = (UINT64)rayCount * sizeof(Result);
    auto out = CreateBuffer(c.g.device.Get(), outSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    // dims lives in a DEFAULT heap and is filled by a GPU copy, exactly like
    // Unreal's DispatchRaysDescBuffer. Buffers promote out of COMMON on their
    // own, so no barriers are needed around it.
    auto dims = CreateBuffer(c.g.device.Get(), 16, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON);
    auto dimsUpload = CreateBuffer(c.g.device.Get(), 16, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    auto dimsReadback = CreateBuffer(c.g.device.Get(), 16, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);
    {
        void* p = nullptr; D3D12_RANGE none{ 0, 0 };
        HR(dimsUpload->Map(0, &none, &p), "map dims upload");
        uint32_t v[4] = { rayCount, 1, 1, 0 };
        std::memcpy(p, v, sizeof(v));
        dimsUpload->Unmap(0, nullptr);
    }

    auto bind = [&](ID3D12StateObject* so) {
        c.g.list->SetPipelineState1(so);
        BindRootsTiming(c.g.list.Get(), rs.Get(), cb->GetGPUVirtualAddress(),
                        c.scene.tlas->GetGPUVirtualAddress(), out->GetGPUVirtualAddress(),
                        dims->GetGPUVirtualAddress());
    };

    // --- baseline: one submit, no split ------------------------------------
    auto runDirect = [&](ID3D12StateObject* so, D3D12_DISPATCH_RAYS_DESC dr, UINT width) {
        c.g.list->CopyBufferRegion(dims.Get(), 0, dimsUpload.Get(), 0, 16);
        bind(so);
        dr.Width = width; dr.Height = 1; dr.Depth = 1;
        c.g.list->DispatchRays(&dr);
        UavBarrier(c.g.list.Get(), out.Get());
        c.g.flush();
    };

    // --- S1: split the list, sync, read back, then dispatch ----------------
    auto runSplit = [&]() {
        // Segment A: the app's work that produces the dimensions, then our
        // readback copy. Closing here is the split.
        c.g.list->CopyBufferRegion(dims.Get(), 0, dimsUpload.Get(), 0, 16);
        c.g.list->CopyBufferRegion(dimsReadback.Get(), 0, dims.Get(), 0, 16);
        c.g.flush();                       // submit + CPU wait: the bubble

        uint32_t got = 0;
        void* rp = nullptr; D3D12_RANGE full{ 0, 16 };
        HR(dimsReadback->Map(0, &full, &rp), "map dims readback");
        std::memcpy(&got, rp, sizeof(got));
        D3D12_RANGE nowrite{ 0, 0 }; dimsReadback->Unmap(0, &nowrite);

        // Segment B: the dispatch we could only now build.
        bind(soPlain.Get());
        auto dr = drPlain; dr.Width = got; dr.Height = 1; dr.Depth = 1;
        c.g.list->DispatchRays(&dr);
        UavBarrier(c.g.list.Get(), out.Get());
        c.g.flush();
    };

    auto timeIt = [&](const char* label, const std::function<void()>& body, double baseline) {
        for (int i = 0; i < 2; ++i) body();          // warm up
        double t0 = NowMs();
        for (int i = 0; i < iters; ++i) body();
        double ms = (NowMs() - t0) / iters;
        if (baseline > 0.0)
            std::printf("   %-34s %8.3f ms   %+6.1f%%\n", label, ms,
                        (ms / baseline - 1.0) * 100.0);
        else
            std::printf("   %-34s %8.3f ms\n", label, ms);
        return ms;
    };

    double base = timeIt("direct DispatchRays (baseline)",
                         [&] { runDirect(soPlain.Get(), drPlain, rayCount); }, 0.0);
    double splitMs = timeIt("S1  split + CPU sync + dispatch",
                            [&] { runSplit(); }, base);
    timeIt("S2  bounded shader, G = 1x",
           [&] { runDirect(soBound.Get(), drBound, rayCount); }, base);
    timeIt("S2  bounded shader, G = 2x",
           [&] { runDirect(soBound.Get(), drBound, rayCount * 2); }, base);
    timeIt("S2  bounded shader, G = 4x",
           [&] { runDirect(soBound.Get(), drBound, rayCount * 4); }, base);
    double over10 = timeIt("S2  bounded shader, G = 10x",
                           [&] { runDirect(soBound.Get(), drBound, rayCount * 10); }, base);

    std::printf("   note: S2 rows do the SAME real ray work as the baseline;\n");
    std::printf("         the extra threads read one uint and return.\n");

    // The absolute numbers belong to this toy scene. These two coefficients do
    // not: one is a property of submit-and-wait, the other of a thread that
    // early-outs. Both carry over to a real frame where the ray work is far
    // heavier, which is the case that actually decides the strategy.
    const double perSplitMs = splitMs - base;
    const double wastedThreads = (double)rayCount * 9.0;   // the 10x row adds 9N
    const double perThreadNs = (over10 - base) * 1e6 / wastedThreads;

    std::printf("\n   derived, and these are the numbers that transfer:\n");
    std::printf("     S1 fixed cost per split        %8.3f ms\n", perSplitMs);
    std::printf("     S2 cost per early-out thread   %8.4f ns\n", perThreadNs);
    std::printf("\n   for a frame with D indirect ray dispatches of A rays each,\n");
    std::printf("   over-dispatched to G*A:\n");
    std::printf("     S1 overhead = D * %.3f ms            (independent of scene cost)\n", perSplitMs);
    std::printf("     S2 overhead = D * (G-1) * A * %.4f ns\n", perThreadNs);
    std::printf("   break-even at G-1 = %.1f  for A = 1M rays per dispatch\n",
                perSplitMs * 1e6 / (perThreadNs * 1048576.0));
}

// ---------------------------------------------------------------------------
// Pipelined timing: what a mid-frame CPU sync actually costs in a renderer that
// keeps the CPU ahead of the GPU.
//
// The -time mode measures the sync round trip on an idle GPU, which is a floor.
// The real damage is different: a renderer overlaps CPU recording of frame N+1
// with GPU execution of frame N, and a mid-frame wait collapses that overlap.
// Throughput then goes from roughly max(CPU, GPU) to roughly CPU + GPU.
//
// So this harness needs both halves to be non-trivial, or there is nothing to
// lose and the sync looks free. GPU work is scaled with filler dispatch passes;
// CPU work is a synthetic busy-wait standing in for recording cost, swept so the
// shape of the curve is visible rather than a single number.
// ---------------------------------------------------------------------------
namespace {
struct Pipe {
    static const int kInFlight = 3;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc[kInFlight];
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 next = 1;
    UINT64 slotVal[kInFlight] = { 0, 0, 0 };
    HANDLE evt = nullptr;
    int slot = 0;

    void init(ID3D12Device5* dev) {
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "pipe queue");
        for (int i = 0; i < kInFlight; ++i)
            HR(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&alloc[i])), "pipe allocator");
        HR(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(),
            nullptr, IID_PPV_ARGS(&list)), "pipe list");
        HR(list->Close(), "pipe list close");
        HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "pipe fence");
        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    void waitFor(UINT64 v) {
        if (fence->GetCompletedValue() >= v) return;
        HR(fence->SetEventOnCompletion(v, evt), "pipe wait");
        WaitForSingleObject(evt, INFINITE);
    }
    // Block only until this slot's previous frame is done, which is what keeps
    // the CPU at most kInFlight frames ahead rather than fully serialized.
    void beginFrame(int frameIndex) {
        slot = frameIndex % kInFlight;
        waitFor(slotVal[slot]);
        HR(alloc[slot]->Reset(), "pipe alloc reset");
        HR(list->Reset(alloc[slot].Get(), nullptr), "pipe list reset");
    }
    void submit() {
        HR(list->Close(), "pipe close");
        ID3D12CommandList* l[] = { list.Get() };
        queue->ExecuteCommandLists(1, l);
        HR(queue->Signal(fence.Get(), next), "pipe signal");
        slotVal[slot] = next++;
    }
    // The split: submit, block until it lands, then keep recording into the same
    // allocator. Legal because the previously recorded list has finished.
    void submitAndWaitThenContinue() {
        submit();
        waitFor(slotVal[slot]);
        HR(list->Reset(alloc[slot].Get(), nullptr), "pipe list reset 2");
    }
    void drain() { waitFor(next - 1); }
};
} // namespace

static void BusyWaitMs(double ms) {
    if (ms <= 0.0) return;
    const double end = NowMs() + ms;
    while (NowMs() < end) { /* stand-in for CPU recording cost */ }
}

static void ProbePipeline(Ctx& c, uint32_t rayCount, int fillerPasses, int frames) {
    uint32_t side = 1;
    while ((uint64_t)(side + 1) * (side + 1) <= rayCount) ++side;
    rayCount = side * side;

    std::printf("\n-- pipelined timing: cost of a mid-frame sync --\n");
    std::printf("   %u rays over %ux%u, %d filler passes, %d frames, %d in flight\n",
                rayCount, side, side, fillerPasses, frames, Pipe::kInFlight);

    auto rs = MakeRootSigTiming(c.g.device.Get());
    std::string err;
    auto lib = c.dxc.tryCompile(kTimingHLSL, L"lib_6_3", { L"BOUNDED=0" }, err);
    if (!lib) { std::printf("   [!] shader: %s\n", err.c_str()); return; }
    ComPtr<ID3D12StateObject> so;
    HRESULT hr = MakeStateObject(c, lib.Get(), false, so, rs.Get());
    if (FAILED(hr)) { std::printf("   [!] state object: %s\n", HrStr(hr).c_str()); return; }
    ComPtr<ID3D12StateObjectProperties> props;
    HR(so.As(&props), "props");
    auto sbtRay  = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"RayGen"));
    auto sbtMiss = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"Miss"));
    const void* ids[2] = { props->GetShaderIdentifier(L"HitGroup"),
                           props->GetShaderIdentifier(L"HitGroupProc") };
    auto sbtHit = MakeSBT(c.g.device.Get(), ids, 2);
    auto dr = MakeDispatchDesc(sbtRay.Get(), sbtMiss.Get(), sbtHit.Get(), 2);

    SceneCB cbData{ side, side, kHalfExtent, kCamZ, kTMin, kTMax, 0, 0 };
    UINT64 cbSize = (sizeof(SceneCB) + 255) & ~255ull;
    auto cb = CreateBuffer(c.g.device.Get(), cbSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    { void* p = nullptr; D3D12_RANGE none{ 0, 0 };
      HR(cb->Map(0, &none, &p), "map cb"); std::memcpy(p, &cbData, sizeof(cbData));
      cb->Unmap(0, nullptr); }

    const UINT64 outSize = (UINT64)rayCount * sizeof(Result);
    auto out = CreateBuffer(c.g.device.Get(), outSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto dims = CreateBuffer(c.g.device.Get(), 16, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON);
    auto dimsUpload = CreateBuffer(c.g.device.Get(), 16, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    auto dimsReadback = CreateBuffer(c.g.device.Get(), 16, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);
    { void* p = nullptr; D3D12_RANGE none{ 0, 0 };
      HR(dimsUpload->Map(0, &none, &p), "map dims");
      uint32_t v[4] = { rayCount, 1, 1, 0 };
      std::memcpy(p, v, sizeof(v)); dimsUpload->Unmap(0, nullptr); }

    Pipe pipe; pipe.init(c.g.device.Get());

    auto bind = [&](ID3D12GraphicsCommandList4* cl) {
        cl->SetPipelineState1(so.Get());
        BindRootsTiming(cl, rs.Get(), cb->GetGPUVirtualAddress(),
                        c.scene.tlas->GetGPUVirtualAddress(), out->GetGPUVirtualAddress(),
                        dims->GetGPUVirtualAddress());
    };
    auto recordFiller = [&](ID3D12GraphicsCommandList4* cl) {
        for (int i = 0; i < fillerPasses; ++i) {
            bind(cl);
            auto d = dr; d.Width = rayCount; d.Height = 1; d.Depth = 1;
            cl->DispatchRays(&d);
            UavBarrier(cl, out.Get());
        }
    };

    // Baseline: one submit per frame, CPU never waits on this frame's GPU work.
    auto runBaseline = [&](double cpuMs) {
        for (int f = 0; f < frames; ++f) {
            pipe.beginFrame(f);
            BusyWaitMs(cpuMs);
            recordFiller(pipe.list.Get());
            pipe.list->CopyBufferRegion(dims.Get(), 0, dimsUpload.Get(), 0, 16);
            bind(pipe.list.Get());
            auto d = dr; d.Width = rayCount; d.Height = 1; d.Depth = 1;
            pipe.list->DispatchRays(&d);
            UavBarrier(pipe.list.Get(), out.Get());
            pipe.submit();
        }
        pipe.drain();
    };

    // S1: identical work, but the CPU blocks mid-frame to read the dimensions.
    // `splits` models an engine with several indirect ray dispatches per frame;
    // the filler is divided between them so total GPU work stays constant.
    auto runSplitN = [&](double cpuMs, int splits) {
        for (int f = 0; f < frames; ++f) {
            pipe.beginFrame(f);
            BusyWaitMs(cpuMs);
            for (int s = 0; s < splits; ++s) {
                for (int i = 0; i < fillerPasses / splits; ++i) {
                    bind(pipe.list.Get());
                    auto d = dr; d.Width = rayCount; d.Height = 1; d.Depth = 1;
                    pipe.list->DispatchRays(&d);
                    UavBarrier(pipe.list.Get(), out.Get());
                }
                pipe.list->CopyBufferRegion(dims.Get(), 0, dimsUpload.Get(), 0, 16);
                pipe.list->CopyBufferRegion(dimsReadback.Get(), 0, dims.Get(), 0, 16);
                pipe.submitAndWaitThenContinue();      // the overlap dies here

                uint32_t got = 0;
                void* rp = nullptr; D3D12_RANGE full{ 0, 16 };
                HR(dimsReadback->Map(0, &full, &rp), "map readback");
                std::memcpy(&got, rp, sizeof(got));
                D3D12_RANGE nowrite{ 0, 0 }; dimsReadback->Unmap(0, &nowrite);

                bind(pipe.list.Get());
                auto d = dr; d.Width = got; d.Height = 1; d.Depth = 1;
                pipe.list->DispatchRays(&d);
                UavBarrier(pipe.list.Get(), out.Get());
            }
            pipe.submit();
        }
        pipe.drain();
    };
    auto runSplit = [&](double cpuMs) { runSplitN(cpuMs, 1); };

    auto measure = [&](const std::function<void(double)>& fn, double cpuMs) {
        fn(cpuMs);                       // warm up
        double t0 = NowMs();
        fn(cpuMs);
        return (NowMs() - t0) / frames;
    };

    std::printf("\n   CPU ms/frame    baseline    with split    overhead\n");
    std::printf("   ------------  ----------  ------------  ----------\n");
    for (double cpuMs : { 0.0, 1.0, 2.0, 4.0, 8.0 }) {
        double b = measure(runBaseline, cpuMs);
        double s = measure(runSplit, cpuMs);
        std::printf("   %10.1f    %8.3f      %8.3f    %+7.1f%%\n",
                    cpuMs, b, s, (s / b - 1.0) * 100.0);
    }
    std::printf("\n   baseline should track max(CPU, GPU); a collapsed pipeline\n");
    std::printf("   tracks CPU + GPU instead. The gap is what the sync really costs.\n");

    // Does the penalty multiply with the number of indirect dispatches per
    // frame, or saturate once the pipeline has already collapsed? This decides
    // whether batching several dispatches behind one sync is worth building.
    const double cpuFixed = 2.0;     // near the worst case, where CPU ~= GPU
    std::printf("\n   splits per frame, CPU fixed at %.1f ms:\n", cpuFixed);
    std::printf("   splits   with split    vs baseline\n");
    std::printf("   ------  ----------  -------------\n");
    double b1 = measure(runBaseline, cpuFixed);
    for (int splits : { 1, 2, 4 }) {
        if (fillerPasses % splits != 0) continue;
        double s = measure([&](double cpu) { runSplitN(cpu, splits); }, cpuFixed);
        std::printf("   %6d    %8.3f      %+7.1f%%\n", splits, s, (s / b1 - 1.0) * 100.0);
    }
    std::printf("   if this is flat, the damage saturates and batching dispatches\n");
    std::printf("   behind one sync buys nothing.\n");
}



// ---------------------------------------------------------------------------
// Two indirect ray dispatches back to back, with no GPU work between them.
//
// The shim defers closing a segment until the application records work that has
// to be ordered after a queued dispatch. So a run of dispatches like this should
// land in ONE segment and share ONE sync, instead of paying a stall each.
//
// The two dispatches write to DIFFERENT output buffers, with only a rebinding
// between them. That makes the test prove two things at once: that both
// dispatches actually ran, and that each carries its own binding snapshot rather
// than sharing the last one set.
// ---------------------------------------------------------------------------
static void ProbeBatchSplit(Ctx& c) {
    std::printf("\n-- two adjacent indirect dispatches: one sync? --\n");

    std::string err;
    auto lib = c.dxc.tryCompile(kLibHLSL, L"lib_6_3", {}, err);
    if (!lib) { std::printf("   [!] library: %s\n", err.c_str()); return; }
    ComPtr<ID3D12StateObject> so;
    if (FAILED(MakeStateObject(c, lib.Get(), false, so))) {
        std::printf("   [!] state object failed\n"); return;
    }
    ComPtr<ID3D12StateObjectProperties> props;
    HR(so.As(&props), "props");
    auto sbtRay  = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"RayGen"));
    auto sbtMiss = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"Miss"));
    const void* hitIds[2] = { props->GetShaderIdentifier(L"HitGroup"),
                              props->GetShaderIdentifier(L"HitGroupProc") };
    auto sbtHit = MakeSBT(c.g.device.Get(), hitIds, 2);
    auto dr = MakeDispatchDesc(sbtRay.Get(), sbtMiss.Get(), sbtHit.Get(), 2);

    D3D12_INDIRECT_ARGUMENT_DESC arg{}; arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    D3D12_COMMAND_SIGNATURE_DESC csd{};
    csd.ByteStride = sizeof(D3D12_DISPATCH_RAYS_DESC);
    csd.NumArgumentDescs = 1; csd.pArgumentDescs = &arg;
    ComPtr<ID3D12CommandSignature> cs;
    if (FAILED(c.g.device->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&cs)))) {
        std::printf("   [!] DISPATCH_RAYS signature unavailable\n"); return;
    }

    auto upArgs = CreateBuffer(c.g.device.Get(), sizeof(dr), D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
    { void* p = nullptr; D3D12_RANGE none{ 0, 0 };
      HR(upArgs->Map(0, &none, &p), "map args"); std::memcpy(p, &dr, sizeof(dr));
      upArgs->Unmap(0, nullptr); }
    auto argsA = CreateBuffer(c.g.device.Get(), sizeof(dr), D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_COMMON);
    auto argsB = CreateBuffer(c.g.device.Get(), sizeof(dr), D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_COMMON);

    // A second output, so the two dispatches are distinguishable.
    auto out2 = CreateBuffer(c.g.device.Get(), c.outSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto rb2 = CreateBuffer(c.g.device.Get(), c.outSize, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);

    c.clear_out();
    auto& cl = c.g.list;
    // Both argument buffers are produced here, BEFORE either dispatch, which is
    // what makes a single sync sufficient for both.
    cl->CopyBufferRegion(argsA.Get(), 0, upArgs.Get(), 0, sizeof(dr));
    cl->CopyBufferRegion(argsB.Get(), 0, upArgs.Get(), 0, sizeof(dr));
    Transition(cl.Get(), argsA.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    Transition(cl.Get(), argsB.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    cl->SetPipelineState1(so.Get());
    BindRoots(cl.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
              c.scene.tlas->GetGPUVirtualAddress(), c.out->GetGPUVirtualAddress());
    cl->ExecuteIndirect(cs.Get(), 1, argsA.Get(), 0, nullptr, 0);

    // Only a rebinding between them: no GPU work, so the segment should stay
    // open and both dispatches should share one sync.
    BindRoots(cl.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
              c.scene.tlas->GetGPUVirtualAddress(), out2->GetGPUVirtualAddress());
    cl->ExecuteIndirect(cs.Get(), 1, argsB.Get(), 0, nullptr, 0);

    c.g.flush();

    uint32_t triA = 0, procA = 0, ignore = 0, unwrittenA = 0;
    c.readback_counts(triA, procA, ignore, unwrittenA, 0);

    Transition(cl.Get(), out2.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyResource(rb2.Get(), out2.Get());
    c.g.flush();
    std::vector<Result> v(kWidth * kHeight);
    void* rp = nullptr; D3D12_RANGE full{ 0, (SIZE_T)c.outSize };
    HR(rb2->Map(0, &full, &rp), "map rb2");
    std::memcpy(v.data(), rp, c.outSize);
    D3D12_RANGE noWrite{ 0, 0 }; rb2->Unmap(0, &noWrite);
    uint32_t triB = 0, procB = 0;
    for (auto& r : v) { if (r.hit == 1) ++triB; if (r.hit == 2) ++procB; }

    std::printf("   dispatch A -> buffer 1   : %u tri + %u proc\n", triA, procA);
    std::printf("   dispatch B -> buffer 2   : %u tri + %u proc\n", triB, procB);
    const bool ok = (triA == 14450 && procA == 2312 && triB == 14450 && procB == 2312);
    std::printf("   RESULT                   : %s\n", ok
        ? "both dispatches ran, each with its own bindings"
        : "DIVERGE, a dispatch was lost or bindings were shared");
    std::printf("   (the log should read \"one sync for 2 dispatches\")\n");

    // Negative control. The same two dispatches, but with a UAV barrier
    // recorded between them. A barrier is ordered work, so it has to end the
    // segment: if this pass ALSO reports one sync, the batching is ignoring
    // ordering and the pass above proved nothing.
    std::printf("\n   -- control: a UAV barrier between them --\n");
    c.clear_out();
    cl->SetPipelineState1(so.Get());
    BindRoots(cl.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
              c.scene.tlas->GetGPUVirtualAddress(), c.out->GetGPUVirtualAddress());
    cl->ExecuteIndirect(cs.Get(), 1, argsA.Get(), 0, nullptr, 0);
    UavBarrier(cl.Get(), c.out.Get());          // ordered work: forces the break
    BindRoots(cl.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
              c.scene.tlas->GetGPUVirtualAddress(), c.out->GetGPUVirtualAddress());
    cl->ExecuteIndirect(cs.Get(), 1, argsB.Get(), 0, nullptr, 0);
    c.g.flush();

    uint32_t triC = 0, procC = 0, ignore2 = 0, unwrittenC = 0;
    c.readback_counts(triC, procC, ignore2, unwrittenC, 0);
    std::printf("   both dispatches -> buffer 1: %u tri + %u proc\n", triC, procC);
    std::printf("   RESULT                   : %s\n",
        (triC == 14450 && procC == 2312) ? "correct across the forced break"
                                         : "DIVERGE");
    std::printf("   (the log should now read \"one sync for 1 dispatch\", twice)\n");

    // What the batching is actually worth. Identical ray work either way, eight
    // dispatches of the same grid; the only difference is whether the shim can
    // keep them in one segment. One confound worth naming: the barrier variant
    // also serialises on the GPU. These segments already run in sequence on a
    // single queue, so that part should be close to free, but it is not zero.
    const int kRuns = 5, kDisp = 8;
    double t[2][kRuns];
    for (int pass = 0; pass < 2; ++pass) {
        for (int r = 0; r < kRuns; ++r) {
            c.g.flush();
            const double t0 = NowMs();
            cl->SetPipelineState1(so.Get());
            for (int i = 0; i < kDisp; ++i) {
                BindRoots(cl.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
                          c.scene.tlas->GetGPUVirtualAddress(),
                          c.out->GetGPUVirtualAddress());
                cl->ExecuteIndirect(cs.Get(), 1, argsA.Get(), 0, nullptr, 0);
                if (pass == 1 && i + 1 < kDisp) UavBarrier(cl.Get(), c.out.Get());
            }
            c.g.flush();
            t[pass][r] = NowMs() - t0;
        }
        std::sort(t[pass], t[pass] + kRuns);
    }
    std::printf("\n   -- what one sync is worth, %d dispatches --\n", kDisp);
    std::printf("   1 segment,  1 sync       : %6.2f ms   (median of %d)\n",
                t[0][kRuns / 2], kRuns);
    std::printf("   %d segments, %d syncs      : %6.2f ms\n",
                kDisp, kDisp, t[1][kRuns / 2]);
    std::printf("   saved                    : %6.2f ms   (%.0f%%)\n",
                t[1][kRuns / 2] - t[0][kRuns / 2],
                100.0 * (t[1][kRuns / 2] - t[0][kRuns / 2]) / t[1][kRuns / 2]);
}

// ---------------------------------------------------------------------------
// Does graphics state survive a command list split?
//
// The split closes the application's command list and opens a fresh one, and a
// fresh list has NO state. The shim replays what the app had set, but replaying
// only compute state is not enough: a draw recorded after the split also needs
// its pipeline state, root signature and parameters, topology, viewports,
// scissors, render targets and vertex buffers.
//
// So: bind all of that, force a split with a GPU-argument indirect ray dispatch,
// then draw. If the replay is complete the triangle appears in the render
// target. If anything is missing the draw is dropped or wrong, and the target
// keeps its clear colour.
// ---------------------------------------------------------------------------
static const char* kGfxVS = R"HLSL(
struct VSIn  { float2 pos : POSITION; };
struct VSOut { float4 pos : SV_Position; };
VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 0, 1); return o; }
)HLSL";

static const char* kGfxPS = R"HLSL(
cbuffer C : register(b0) { float4 tint; };
float4 main(float4 pos : SV_Position) : SV_Target { return tint; }
)HLSL";

static void ProbeGfxSplit(Ctx& c) {
    std::printf("\n-- does graphics state survive a split? --\n");

    std::string err;
    auto vs = c.dxc.tryCompile(kGfxVS, L"vs_6_0", {}, err, L"main");
    if (!vs) { std::printf("   [!] VS: %s\n", err.c_str()); return; }
    auto ps = c.dxc.tryCompile(kGfxPS, L"ps_6_0", {}, err, L"main");
    if (!ps) { std::printf("   [!] PS: %s\n", err.c_str()); return; }

    // Graphics root signature: four root constants, so root parameters are
    // exercised too and not just the pipeline state.
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 4;
    rp.Constants.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, rerr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rerr))) {
        std::printf("   [!] graphics root signature failed\n"); return;
    }
    ComPtr<ID3D12RootSignature> grs;
    HR(c.g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&grs)), "CreateRootSignature(gfx)");

    D3D12_INPUT_ELEMENT_DESC elem{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = grs.Get();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { &elem, 1 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(c.g.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) {
        std::printf("   [!] graphics PSO failed\n"); return;
    }

    // A triangle covering the whole target.
    const float verts[6] = { -1.0f, -3.0f,  -1.0f, 1.0f,  3.0f, 1.0f };
    auto vb = CreateBuffer(c.g.device.Get(), sizeof(verts), D3D12_HEAP_TYPE_UPLOAD,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
    { void* p = nullptr; D3D12_RANGE none{ 0, 0 };
      HR(vb->Map(0, &none, &p), "map vb"); std::memcpy(p, verts, sizeof(verts));
      vb->Unmap(0, nullptr); }
    D3D12_VERTEX_BUFFER_VIEW vbv{ vb->GetGPUVirtualAddress(), sizeof(verts), sizeof(float) * 2 };

    const UINT kRT = 64;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kRT; td.Height = kRT; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{}; clear.Format = td.Format;
    ComPtr<ID3D12Resource> rt;
    HR(c.g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&rt)), "rt");

    D3D12_DESCRIPTOR_HEAP_DESC rtvd{};
    rtvd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtvd.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    HR(c.g.device->CreateDescriptorHeap(&rtvd, IID_PPV_ARGS(&rtvHeap)), "rtv heap");
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    c.g.device->CreateRenderTargetView(rt.Get(), nullptr, rtv);

    // Readback for the result.
    const UINT rowPitch = (kRT * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                          ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    auto rb = CreateBuffer(c.g.device.Get(), (UINT64)rowPitch * kRT,
                           D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    // The indirect ray dispatch that forces the split, with GPU-written args.
    ComPtr<ID3D12StateObject> so;
    std::string why;
    auto lib = c.dxc.tryCompile(kLibHLSL, L"lib_6_3", {}, why);
    if (!lib) { std::printf("   [!] rt library: %s\n", why.c_str()); return; }
    if (FAILED(MakeStateObject(c, lib.Get(), false, so))) {
        std::printf("   [!] rt state object failed\n"); return;
    }
    ComPtr<ID3D12StateObjectProperties> props;
    HR(so.As(&props), "props");
    auto sbtRay  = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"RayGen"));
    auto sbtMiss = MakeSBT(c.g.device.Get(), props->GetShaderIdentifier(L"Miss"));
    const void* hitIds[2] = { props->GetShaderIdentifier(L"HitGroup"),
                              props->GetShaderIdentifier(L"HitGroupProc") };
    auto sbtHit = MakeSBT(c.g.device.Get(), hitIds, 2);
    auto dr = MakeDispatchDesc(sbtRay.Get(), sbtMiss.Get(), sbtHit.Get(), 2);

    D3D12_INDIRECT_ARGUMENT_DESC arg{}; arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    D3D12_COMMAND_SIGNATURE_DESC csd{};
    csd.ByteStride = sizeof(D3D12_DISPATCH_RAYS_DESC);
    csd.NumArgumentDescs = 1; csd.pArgumentDescs = &arg;
    ComPtr<ID3D12CommandSignature> cs;
    if (FAILED(c.g.device->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&cs)))) {
        std::printf("   [!] DISPATCH_RAYS signature unavailable, cannot force a split\n");
        return;
    }
    auto upArgs = CreateBuffer(c.g.device.Get(), sizeof(dr), D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ);
    { void* p = nullptr; D3D12_RANGE none{ 0, 0 };
      HR(upArgs->Map(0, &none, &p), "map args"); std::memcpy(p, &dr, sizeof(dr));
      upArgs->Unmap(0, nullptr); }
    auto gpuArgs = CreateBuffer(c.g.device.Get(), sizeof(dr), D3D12_HEAP_TYPE_DEFAULT,
                                D3D12_RESOURCE_STATE_COMMON);

    // --- one recording: bind graphics, split in the middle, then draw --------
    auto& cl = c.g.list;
    const FLOAT black[4] = { 0, 0, 0, 1 };
    cl->ClearRenderTargetView(rtv, black, 0, nullptr);

    D3D12_VIEWPORT vp{ 0, 0, (FLOAT)kRT, (FLOAT)kRT, 0, 1 };
    D3D12_RECT sr{ 0, 0, (LONG)kRT, (LONG)kRT };
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
    cl->SetPipelineState(pso.Get());
    cl->SetGraphicsRootSignature(grs.Get());
    const float tint[4] = { 0.0f, 1.0f, 0.0f, 1.0f };     // green
    cl->SetGraphicsRoot32BitConstants(0, 4, tint, 0);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->IASetVertexBuffers(0, 1, &vbv);

    // The split. Everything above must survive it.
    cl->CopyBufferRegion(gpuArgs.Get(), 0, upArgs.Get(), 0, sizeof(dr));
    Transition(cl.Get(), gpuArgs.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    cl->SetPipelineState1(so.Get());
    BindRoots(cl.Get(), c.rs.Get(), c.cb->GetGPUVirtualAddress(),
              c.scene.tlas->GetGPUVirtualAddress(), c.out->GetGPUVirtualAddress());
    cl->ExecuteIndirect(cs.Get(), 1, gpuArgs.Get(), 0, nullptr, 0);

    // SetPipelineState1 replaces the bound pipeline, so ANY app has to re-bind
    // its graphics PSO before drawing again, split or no split. Re-bind exactly
    // that one thing and nothing else: everything the draw still needs, the
    // render target, viewport, scissor, topology, vertex buffer, root signature
    // and root constants, has to have survived on its own.
    cl->SetPipelineState(pso.Get());

    // The draw that only works if the rest of the graphics state came back.
    cl->DrawInstanced(3, 1, 0, 0);

    Transition(cl.Get(), rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = td.Format;
    dst.PlacedFootprint.Footprint.Width = kRT;
    dst.PlacedFootprint.Footprint.Height = kRT;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = rt.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    c.g.flush();

    uint8_t* p = nullptr;
    D3D12_RANGE all{ 0, (SIZE_T)rowPitch * kRT };
    HR(rb->Map(0, &all, (void**)&p), "map rb");
    uint32_t green = 0, black_ = 0, other = 0;
    for (UINT y = 0; y < kRT; ++y) {
        for (UINT x = 0; x < kRT; ++x) {
            const uint8_t* px = p + (size_t)y * rowPitch + (size_t)x * 4;
            if (px[0] < 16 && px[1] > 200 && px[2] < 16) ++green;
            else if (px[0] < 16 && px[1] < 16 && px[2] < 16) ++black_;
            else ++other;
        }
    }
    D3D12_RANGE noWrite{ 0, 0 }; rb->Unmap(0, &noWrite);

    const UINT total = kRT * kRT;
    std::printf("   render target after the split: %u green, %u black, %u other (of %u)\n",
                green, black_, other, total);
    std::printf("   RESULT                       : %s\n",
                (green == total) ? "graphics state survived the split"
                                 : "DIVERGE, the draw did not land");
}

// ---------------------------------------------------------------------------
// Hook test: drive a REAL ExecuteIndirect on Tier 1.0 hardware.
//
// The shim hooks ExecuteIndirect in the command list vtable. Its install-time
// self-test proves the slot index is right, but nothing in the rest of this
// probe ever calls ExecuteIndirect on Tier 1.0, because CreateCommandSignature
// refuses DISPATCH_RAYS there. So a broken forward would stay latent until the
// first real workload hit it.
//
// A plain DISPATCH command signature is supported on every tier, so this uses
// one to put genuine traffic through the hook and check the result is correct.
// ---------------------------------------------------------------------------
static const char* kHookTestCS = R"HLSL(
RWStructuredBuffer<uint> outBuf : register(u0);
[numthreads(1,1,1)]
void main(uint3 tid : SV_DispatchThreadID) { outBuf[tid.x] = 0xC0FFEE + tid.x; }
)HLSL";

static void ProbeHookTest(Ctx& c) {
    std::printf("\n-- real ExecuteIndirect through a DISPATCH signature --\n");

    std::string err;
    auto cs = c.dxc.tryCompile(kHookTestCS, L"cs_6_0", {}, err);
    if (!cs) { std::printf("   [!] compute shader: %s\n", err.c_str()); return; }

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    param.Descriptor.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 1; rd.pParameters = &param;
    ComPtr<ID3DBlob> blob, rerr;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rerr);
    if (FAILED(hr)) { std::printf("   [!] root sig %s\n", HrStr(hr).c_str()); return; }
    ComPtr<ID3D12RootSignature> rs;
    HR(c.g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&rs)), "CreateRootSignature(hooktest)");

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    ComPtr<ID3D12PipelineState> pso;
    hr = c.g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) { std::printf("   [!] compute PSO %s\n", HrStr(hr).c_str()); return; }

    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC csd{};
    csd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    csd.NumArgumentDescs = 1; csd.pArgumentDescs = &arg;
    ComPtr<ID3D12CommandSignature> sig;
    hr = c.g.device->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&sig));
    std::printf("   CreateCommandSignature(DISPATCH) : %s\n",
                SUCCEEDED(hr) ? "OK" : HrStr(hr).c_str());
    if (FAILED(hr)) return;

    const UINT kThreads = 8;
    auto out = CreateBuffer(c.g.device.Get(), kThreads * sizeof(uint32_t),
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto rb = CreateBuffer(c.g.device.Get(), kThreads * sizeof(uint32_t),
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto argBuf = CreateBuffer(c.g.device.Get(), sizeof(D3D12_DISPATCH_ARGUMENTS),
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    { void* p = nullptr; D3D12_RANGE none{ 0, 0 };
      HR(argBuf->Map(0, &none, &p), "map args");
      D3D12_DISPATCH_ARGUMENTS da{ kThreads, 1, 1 };
      std::memcpy(p, &da, sizeof(da)); argBuf->Unmap(0, nullptr); }

    // Characterise how stable these vtables are, because vtable hooking only
    // works if the pointer you patched is the one still in use at call time.
    auto vt = [](void* obj) { return *reinterpret_cast<void**>(obj); };
    std::printf("   queue vtable                     : %p\n", vt(c.g.queue.Get()));
    std::printf("   list vtable now (recording)      : %p\n", vt(c.g.list.Get()));
    c.g.list->Close();
    std::printf("   list vtable after Close          : %p\n", vt(c.g.list.Get()));
    HR(c.g.list->Reset(c.g.alloc.Get(), nullptr), "reset for hooktest");
    std::printf("   list vtable after Reset          : %p\n", vt(c.g.list.Get()));
    std::printf("   (image addresses look like 00007FF...; heap ones do not)\n");

    c.g.list->SetComputeRootSignature(rs.Get());
    c.g.list->SetPipelineState(pso.Get());
    c.g.list->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
    c.g.list->ExecuteIndirect(sig.Get(), 1, argBuf.Get(), 0, nullptr, 0);
    UavBarrier(c.g.list.Get(), out.Get());
    Transition(c.g.list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.g.list->CopyResource(rb.Get(), out.Get());
    c.g.flush();

    std::vector<uint32_t> got(kThreads);
    void* rp = nullptr; D3D12_RANGE full{ 0, kThreads * sizeof(uint32_t) };
    HR(rb->Map(0, &full, &rp), "map readback");
    std::memcpy(got.data(), rp, kThreads * sizeof(uint32_t));
    D3D12_RANGE nowrite{ 0, 0 }; rb->Unmap(0, &nowrite);

    bool ok = true;
    for (UINT i = 0; i < kThreads; ++i) if (got[i] != 0xC0FFEE + i) ok = false;
    std::printf("   ExecuteIndirect result           : %s (first value 0x%X, expected 0x%X)\n",
                ok ? "CORRECT" : "WRONG", got[0], 0xC0FFEE);
    std::printf("   -> with the proxy present, the log should show the hook\n");
    std::printf("      intercepting this call and forwarding it intact.\n");
}

// ---------------------------------------------------------------------------
// Is the command queue vtable shared and stable?
//
// The hybrid design hooks ExecuteCommandLists in the queue vtable. That is only
// sound if the vtable is shared between queues rather than per object, and does
// not change under the object's feet the way a command list's does. Command
// lists turned out to use a per-object heap vtable, so this is not a safe
// assumption to make twice.
// ---------------------------------------------------------------------------
static void ProbeQueueVTable(Ctx& c) {
    std::printf("\n-- is the command queue vtable shared and stable? --\n");
    auto vt = [](void* o) { return *reinterpret_cast<void**>(o); };

    struct QT { D3D12_COMMAND_LIST_TYPE type; const char* name; };
    const QT types[] = {
        { D3D12_COMMAND_LIST_TYPE_DIRECT,  "DIRECT " },
        { D3D12_COMMAND_LIST_TYPE_COMPUTE, "COMPUTE" },
        { D3D12_COMMAND_LIST_TYPE_COPY,    "COPY   " },
    };

    std::vector<ComPtr<ID3D12CommandQueue>> keep;
    void* firstDirect = nullptr;
    for (const auto& t : types) {
        for (int i = 0; i < 2; ++i) {
            D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = t.type;
            ComPtr<ID3D12CommandQueue> q;
            if (FAILED(c.g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) continue;
            void* v = vt(q.Get());
            bool image = ((uintptr_t)v >> 40) != 0 && ((uintptr_t)v & 0xFFFF000000000000ull) == 0
                         && (uintptr_t)v > 0x00007F0000000000ull;
            std::printf("   %s #%d vtable %p  %s\n", t.name, i, v,
                        image ? "(image)" : "(heap)");
            if (t.type == D3D12_COMMAND_LIST_TYPE_DIRECT && i == 0) firstDirect = v;
            keep.push_back(q);
        }
    }

    // Now use one, the way an app would, and re-read the pointer. A command list
    // swaps its vtable somewhere between creation and recording; check a queue
    // does not do the same across submit and signal.
    if (!keep.empty()) {
        ID3D12CommandQueue* q = keep[0].Get();
        void* before = vt(q);
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        HR(c.g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
        HR(c.g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        HR(list->Close(), "close");
        ID3D12CommandList* ls[] = { list.Get() };
        q->ExecuteCommandLists(1, ls);
        ComPtr<ID3D12Fence> fence;
        HR(c.g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        q->Signal(fence.Get(), 1);
        void* after = vt(q);
        std::printf("   after ExecuteCommandLists+Signal: %p  %s\n", after,
                    (after == before) ? "UNCHANGED" : "CHANGED");
        std::printf("   two DIRECT queues share a vtable : %s\n",
                    (keep.size() > 1 && vt(keep[0].Get()) == vt(keep[1].Get())) ? "yes" : "NO");
        (void)firstDirect;
    }
    std::printf("   -> hooking the queue vtable is sound only if shared and unchanged.\n");
}

// ---------------------------------------------------------------------------
// Experiment: will DXGI accept a wrapped ID3D12CommandQueue?
//
// This decides the shape of the whole indirect DispatchRays implementation.
// Intercepting ExecuteIndirect means wrapping the command list, which forces
// wrapping the queue, and the app hands its queue straight to
// IDXGIFactory::CreateSwapChainForHwnd (DeviceResources.cpp:321 in the
// Microsoft sample). We proxy d3d12.dll only. If DXGI refuses a foreign queue,
// the shim also needs a dxgi.dll proxy to unwrap at that boundary, which is a
// second proxy DLL and a whole extra export table.
//
// The stub below is a genuine forwarding wrapper, not a null implementation,
// because that is exactly what the shim would hand over.
// ---------------------------------------------------------------------------
namespace {
class QueueWrapper : public ID3D12CommandQueue {
public:
    explicit QueueWrapper(ID3D12CommandQueue* real) : m_real(real), m_refs(1) { m_real->AddRef(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        // Answer for ourselves on the interfaces an app holds, which is what
        // makes this a realistic test rather than a trivially-passing one.
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
            riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
            riid == __uuidof(ID3D12CommandQueue)) {
            AddRef(); *ppv = static_cast<ID3D12CommandQueue*>(this); return S_OK;
        }
        return m_real->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&m_refs);
        if (n == 0) { m_real->Release(); delete this; }
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override { return m_real->GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override { return m_real->SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* d) override { return m_real->SetPrivateDataInterface(g, d); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override { return m_real->SetName(n); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppv) override { return m_real->GetDevice(riid, ppv); }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* r, UINT n, const D3D12_TILED_RESOURCE_COORDINATE* a, const D3D12_TILE_REGION_SIZE* b, ID3D12Heap* h, UINT nr, const D3D12_TILE_RANGE_FLAGS* f, const UINT* o, const UINT* c, D3D12_TILE_MAPPING_FLAGS fl) override { m_real->UpdateTileMappings(r, n, a, b, h, nr, f, o, c, fl); }
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* d, const D3D12_TILED_RESOURCE_COORDINATE* dc, ID3D12Resource* s, const D3D12_TILED_RESOURCE_COORDINATE* sc, const D3D12_TILE_REGION_SIZE* rs, D3D12_TILE_MAPPING_FLAGS f) override { m_real->CopyTileMappings(d, dc, s, sc, rs, f); }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT n, ID3D12CommandList* const* l) override { m_real->ExecuteCommandLists(n, l); }
    void STDMETHODCALLTYPE SetMarker(UINT m, const void* d, UINT s) override { m_real->SetMarker(m, d, s); }
    void STDMETHODCALLTYPE BeginEvent(UINT m, const void* d, UINT s) override { m_real->BeginEvent(m, d, s); }
    void STDMETHODCALLTYPE EndEvent() override { m_real->EndEvent(); }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* f, UINT64 v) override { return m_real->Signal(f, v); }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* f, UINT64 v) override { return m_real->Wait(f, v); }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* f) override { return m_real->GetTimestampFrequency(f); }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* g, UINT64* c) override { return m_real->GetClockCalibration(g, c); }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return m_real->GetDesc(); }
private:
    ~QueueWrapper() = default;
    ID3D12CommandQueue* m_real;
    LONG m_refs;
};
} // namespace

static void ProbeDxgiQueue(Ctx& c) {
    std::printf("\n-- can DXGI swap-chain on a WRAPPED command queue? --\n");

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { std::printf("   CreateDXGIFactory2 %s\n", HrStr(hr).c_str()); return; }

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Dxr11ProbeWnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"Dxr11ProbeWnd", L"probe", WS_OVERLAPPEDWINDOW,
                                0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { std::printf("   could not create a window\n"); return; }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 64; sd.Height = 64;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SampleDesc.Count = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    // Control: the real queue, to prove the rest of the setup is sound.
    ComPtr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForHwnd(c.g.queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    std::printf("   real queue     : %s\n", SUCCEEDED(hr) ? "OK" : HrStr(hr).c_str());
    sc1.Reset();

    // Creation alone is NOT the question. An earlier version of this probe
    // stopped there and concluded a wrapped queue was fine, which was wrong:
    // creation succeeds and Present is what fails. Always present a few frames.
    ShowWindow(hwnd, SW_SHOWNA);
    auto presentLoop = [&](IDXGISwapChain1* sc, const char* label) {
        for (int i = 0; i < 3; ++i) {
            HRESULT ph = sc->Present(0, 0);
            if (FAILED(ph)) {
                std::printf("   %s Present[%d] : %s\n", label, i, HrStr(ph).c_str());
                return ph;
            }
            MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
        std::printf("   %s Present    : OK x3\n", label);
        return S_OK;
    };

    ComPtr<IDXGISwapChain1> scReal;
    hr = factory->CreateSwapChainForHwnd(c.g.queue.Get(), hwnd, &sd, nullptr, nullptr, &scReal);
    if (SUCCEEDED(hr)) presentLoop(scReal.Get(), "real queue    ");
    scReal.Reset();

    // The actual question.
    auto* wrapped = new QueueWrapper(c.g.queue.Get());
    ComPtr<IDXGISwapChain1> sc2;
    hr = factory->CreateSwapChainForHwnd(wrapped, hwnd, &sd, nullptr, nullptr, &sc2);
    std::printf("   wrapped queue create : %s\n", SUCCEEDED(hr) ? "OK" : HrStr(hr).c_str());
    HRESULT ph = E_FAIL;
    if (SUCCEEDED(hr)) ph = presentLoop(sc2.Get(), "wrapped queue ");

    if (SUCCEEDED(hr) && SUCCEEDED(ph)) {
        std::printf("   -> a wrapped queue survives creation AND present.\n");
    } else {
        std::printf("   -> a wrapped queue does NOT work end to end. Wrapping the queue\n");
        std::printf("      requires proxying dxgi.dll to unwrap at this boundary, or the\n");
        std::printf("      queue must not be wrapped at all.\n");
    }
    sc2.Reset();
    wrapped->Release();
    DestroyWindow(hwnd);
}

// ---------------------------------------------------------------------------
static ComPtr<IDXGIAdapter1> PickAdapter(IDXGIFactory6* factory, Adapter which) {
    ComPtr<IDXGIAdapter1> adapter;
    if (which == Adapter::Warp) {
        HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
        return adapter;
    }
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))
            != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        return adapter;
    }
    throw std::runtime_error("no hardware adapter found");
}

static void RunAdapter(Adapter which) {
    Ctx c;
    c.which = which;
    c.name = (which == Adapter::Warp) ? "WARP" : "hardware";
    std::printf("\n=====================================================\n");
    std::printf("=== adapter: %s\n", c.name);
    std::printf("=====================================================\n");

    if (g_debug) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    ComPtr<IDXGIFactory6> factory;
    HR(CreateDXGIFactory2(g_debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory)),
       "CreateDXGIFactory2");
    auto adapt = PickAdapter(factory.Get(), which);
    DXGI_ADAPTER_DESC1 ad{}; adapt->GetDesc1(&ad);
    std::printf("    %S\n", ad.Description);

    ComPtr<ID3D12Device5> device;
    HR(D3D12CreateDevice(adapt.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
       "D3D12CreateDevice");

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    HR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)),
       "CheckFeatureSupport(OPTIONS5)");
    c.tier = o5.RaytracingTier;

    // Walk down from 6.7 to find the highest model the runtime will admit to.
    for (D3D_SHADER_MODEL m : { D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
                                D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4,
                                D3D_SHADER_MODEL_6_3 }) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{ m };
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            c.sm = sm.HighestShaderModel; break;
        }
    }

    ComPtr<ID3D12Device7> dev7;
    c.hasDevice7 = SUCCEEDED(device.As(&dev7));

    if (g_debug) {
        // Deliberately no SetBreakOnSeverity: a probe records an error and
        // carries on, it does not die at the first one.
        if (SUCCEEDED(device.As(&c.info))) std::printf("    debug layer: on\n");
        else std::printf("    debug layer: requested, ID3D12InfoQueue unavailable\n");
    }

    if (c.tier < D3D12_RAYTRACING_TIER_1_0) {
        std::printf("    adapter reports no raytracing support; skipping probes\n");
        return;
    }

    c.g.init(device.Get());
    c.dxc.init();
    c.scene = BuildScene(c.g);
    c.rs = MakeRootSig(device.Get());

    SceneCB cbData{ kWidth, kHeight, kHalfExtent, kCamZ, kTMin, kTMax, 0, 0 };
    UINT64 cbSize = (sizeof(SceneCB) + 255) & ~255ull;
    c.cb = CreateBuffer(device.Get(), cbSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(c.cb->Map(0, &none, &p), "map cb"); std::memcpy(p, &cbData, sizeof(cbData));
    c.cb->Unmap(0, nullptr);

    c.outSize = (UINT64)kWidth * kHeight * sizeof(Result);
    c.out = CreateBuffer(device.Get(), c.outSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    c.readback = CreateBuffer(device.Get(), c.outSize, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);

    ProbeCaps(c);

    std::string err;
    auto lib = c.dxc.tryCompile(kLibHLSL, L"lib_6_3", {}, err);
    if (!lib) { std::printf("\n[!] base library failed to compile: %s\n", err.c_str()); return; }
    auto addLib = c.dxc.tryCompile(kAddLibHLSL, L"lib_6_3", {}, err);
    if (!addLib) { std::printf("\n[!] addition library failed to compile: %s\n", err.c_str()); return; }

    if (g_batchSplit) {
        ProbeBatchSplit(c);                             c.drain("batchsplit");
        return;
    }
    if (g_gfxSplit) {
        ProbeGfxSplit(c);                               c.drain("gfxsplit");
        return;
    }
    if (g_queueVTable) {
        ProbeQueueVTable(c);                            c.drain("queuevtable");
        return;
    }
    if (g_hookTest) {
        ProbeHookTest(c);                               c.drain("hooktest");
        return;
    }
    if (g_dxgiQueue) {
        ProbeDxgiQueue(c);                              c.drain("dxgiqueue");
        return;
    }
    if (g_pipeline) {
        ProbePipeline(c, g_rayCount, g_filler, g_frames); c.drain("pipeline");
        return;
    }
    if (g_timing) {
        ProbeTiming(c, g_rayCount, g_iters);            c.drain("timing");
        return;
    }
    ProbeIndirect(c, lib.Get());                        c.drain("indirect");
    ProbeAddToStateObject(c, lib.Get(), addLib.Get());  c.drain("addto");
    ProbeRayFlags(c);                                   c.drain("rayflags");
}

int main(int argc, char** argv) {
    // Unbuffered: this probe deliberately provokes crashes, and a lost stdout
    // buffer hides the line that says how far it got.
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        bool warp = true, hw = true;
        const char* pick = nullptr;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "-debug") == 0) g_debug = true;
            else if (std::strcmp(argv[i], "-time") == 0) g_timing = true;
            else if (std::strcmp(argv[i], "-pipeline") == 0) g_pipeline = true;
            else if (std::strcmp(argv[i], "-dxgiqueue") == 0) g_dxgiQueue = true;
            else if (std::strcmp(argv[i], "-hooktest") == 0) g_hookTest = true;
            else if (std::strcmp(argv[i], "-queuevtable") == 0) g_queueVTable = true;
            else if (std::strcmp(argv[i], "-gfxsplit") == 0) g_gfxSplit = true;
            else if (std::strcmp(argv[i], "-batchsplit") == 0) g_batchSplit = true;
            else if (std::strcmp(argv[i], "-gpuinst") == 0) g_gpuInst = true;
            else if (std::strcmp(argv[i], "-filler") == 0 && i + 1 < argc) g_filler = std::atoi(argv[++i]);
            else if (std::strcmp(argv[i], "-frames") == 0 && i + 1 < argc) g_frames = std::atoi(argv[++i]);
            else if (std::strcmp(argv[i], "-rays") == 0 && i + 1 < argc) g_rayCount = (uint32_t)std::strtoul(argv[++i], nullptr, 10);
            else if (std::strcmp(argv[i], "-iters") == 0 && i + 1 < argc) g_iters = std::atoi(argv[++i]);
            else pick = argv[i];
        }
        if (pick) {
            warp = std::strcmp(pick, "warp") == 0;
            hw   = std::strcmp(pick, "hw") == 0;
            if (!warp && !hw) {
                std::fprintf(stderr, "usage: tier11probe.exe [warp|hw] [-debug] [-time [-rays N] [-iters N]]\n");
                return 2;
            }
        }
        if (warp) RunAdapter(Adapter::Warp);
        if (hw)   RunAdapter(Adapter::Hardware);
        std::printf("\n=== probe complete ===\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FATAL] %s\n", e.what());
        return 1;
    }
}
