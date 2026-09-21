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
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

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
                                std::string& err) {
        DxcBuffer buf{ src, std::strlen(src), DXC_CP_UTF8 };
        std::vector<const wchar_t*> args = { L"-T", target };
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

    auto instBuf = CreateBuffer(g.device.Get(), sizeof(inst), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    HR(instBuf->Map(0, &none, &p), "map inst"); std::memcpy(p, inst, sizeof(inst));
    instBuf->Unmap(0, nullptr);

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
                               ComPtr<ID3D12StateObject>& so) {
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

    ID3D12RootSignature* rsPtr = c.rs.Get();
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
    std::printf("   ExecuteIndirect          : %u tri + %u proc hits, %u unwritten\n", indTri, indProc, unwritten);
    std::printf("   RESULT                   : %s\n",
        (indTri == directTri && indProc == directProc && unwritten == 0) ? "MATCH" : "DIVERGE");
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

    ProbeIndirect(c, lib.Get());                        c.drain("indirect");
    ProbeAddToStateObject(c, lib.Get(), addLib.Get());  c.drain("addto");
    ProbeRayFlags(c);                                   c.drain("rayflags");
}

int main(int argc, char** argv) {
    try {
        bool warp = true, hw = true;
        const char* pick = nullptr;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "-debug") == 0) g_debug = true;
            else pick = argv[i];
        }
        if (pick) {
            warp = std::strcmp(pick, "warp") == 0;
            hw   = std::strcmp(pick, "hw") == 0;
            if (!warp && !hw) {
                std::fprintf(stderr, "usage: tier11probe.exe [warp|hw] [-debug]\n");
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
