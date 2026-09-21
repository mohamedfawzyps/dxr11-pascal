// Phase 2 - hand-lowering test (opaque closest-hit).
//
// One triangle, one BLAS, one TLAS. A grid of orthographic rays is traced two
// ways over the SAME acceleration structure, and the per-ray results are diffed.
//
//   A (ground truth): compute shader using RayQuery<RAY_FLAG_FORCE_OPAQUE>,
//                     run on WARP (Tier 1.1).
//   B (lowered):      raygen shader using TraceRay + closest-hit + miss,
//                     run on the hardware GPU (GTX 1070, Tier 1.0).
//
// Both write the same 16-byte record per ray: { float t; float bx; float by;
// uint hit; }. If A and B agree within tolerance, the RayQuery -> TraceRay
// lowering is sound for this pattern.
//
// Usage:
//   raytest.exe                 run A on WARP -> a.bin, B on hardware -> b.bin,
//                               then diff.
//   raytest.exe warp rayquery out.bin      run one trial and dump it.
//   raytest.exe hw   traceray  out.bin
//   raytest.exe warp traceray  out.bin     (traceray also works on WARP)
//   raytest.exe diff a.bin b.bin           diff two dumps.
//
// Build: build_phase2.bat  (see that file). Needs the DirectX Agility SDK
// headers/runtime and DXC (dxcompiler.dll + dxil.dll) on PATH.
//
// NOTE: written without a Windows build environment to hand; treat the first
// compile as an iteration pass.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

// --- Agility SDK opt-in. D3D12Core.dll must live in .\D3D12\ next to the exe.
#if defined(D3D12_SDK_VERSION) && !defined(DXRTEST_AGILITY_VERSION)
  #define DXRTEST_AGILITY_VERSION D3D12_SDK_VERSION
#endif
#ifndef DXRTEST_AGILITY_VERSION
  #define DXRTEST_AGILITY_VERSION 619u   // matches SDK 1.619.x; override with -DDXRTEST_AGILITY_VERSION=NNN
#endif
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = DXRTEST_AGILITY_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

// ---------------------------------------------------------------------------
// Scene / grid constants. Ortho camera looks down -Z from z=camZ; the triangle
// sits at z=0, so a hit has t == camZ.
static const uint32_t kWidth  = 256;
static const uint32_t kHeight = 256;
static const float kHalfExtent = 1.5f;   // ray origins span [-1.5, 1.5]^2
static const float kCamZ = 2.0f;
static const float kTMin = 0.0f;
static const float kTMax = 10.0f;

#pragma pack(push, 1)
struct Result { float t; float bx; float by; uint32_t hit; };  // 16 bytes
struct SceneCB {                                               // 32 bytes, b0
    uint32_t width; uint32_t height;
    float halfExtent; float camZ; float tMin; float tMax;
    float pad0; float pad1;
};
struct FileHeader { uint32_t magic; uint32_t width; uint32_t height; uint32_t rec; };
#pragma pack(pop)
static const uint32_t kMagic = 0x31425452; // "RTB1"

enum class Adapter { Warp, Hardware };
enum class Method  { RayQuery, TraceRay };
enum class Pattern { Opaque, Alpha };

// ---------------------------------------------------------------------------
static void Throw(const char* what, HRESULT hr) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s (hr=0x%08lx)", what, (unsigned long)hr);
    throw std::runtime_error(buf);
}
static void HR(HRESULT hr, const char* what) { if (FAILED(hr)) Throw(what, hr); }

// --- Shared HLSL sources ---------------------------------------------------
static const char* kRayQueryHLSL = R"HLSL(
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    float u = ((tid.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((tid.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
    q.Proceed();  // FORCE_OPAQUE: never yields a candidate, so one call suffices.

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t = q.CommittedRayT();
        float2 b = q.CommittedTriangleBarycentrics();
        r.bx = b.x; r.by = b.y; r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
)HLSL";

static const char* kTraceRayHLSL = R"HLSL(
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
    TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, ray, p);

    Result r; r.t = p.t; r.bx = p.bary.x; r.by = p.bary.y; r.hit = p.hit;
    outBuf[idx.y * width + idx.x] = r;
}
[shader("closesthit")]
void ClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
    p.t = RayTCurrent(); p.bary = attr.barycentrics; p.hit = 1;
}
[shader("miss")]
void Miss(inout Payload p) { p.hit = 0; }
)HLSL";

// Alpha-tested variants. Geometry is non-opaque; a shared alphaTest() decides
// per-candidate accept/reject. In RayQuery this is the Proceed() loop body; in
// TraceRay it is the generated any-hit shader. This is the core lowering check:
// accept == fall off the end of any-hit, reject == IgnoreHit().
static const char* kRayQueryAlphaHLSL = R"HLSL(
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

bool alphaTest(float2 b) { return frac(b.x * 4.0) < 0.5; }

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    float u = ((tid.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((tid.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {                       // loop body == any-hit shader
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            if (alphaTest(q.CandidateTriangleBarycentrics()))
                q.CommitNonOpaqueTriangleHit(); // accept
            // else: reject (do nothing)
        }
    }
    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t = q.CommittedRayT();
        float2 b = q.CommittedTriangleBarycentrics();
        r.bx = b.x; r.by = b.y; r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
)HLSL";

static const char* kTraceRayAlphaHLSL = R"HLSL(
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);
struct Payload { float t; float2 bary; uint hit; };

bool alphaTest(float2 b) { return frac(b.x * 4.0) < 0.5; }

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
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);

    Result r; r.t = p.t; r.bx = p.bary.x; r.by = p.bary.y; r.hit = p.hit;
    outBuf[idx.y * width + idx.x] = r;
}
[shader("anyhit")]
void AnyHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
    if (!alphaTest(attr.barycentrics)) IgnoreHit();  // reject; else accept
}
[shader("closesthit")]
void ClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
    p.t = RayTCurrent(); p.bary = attr.barycentrics; p.hit = 1;
}
[shader("miss")]
void Miss(inout Payload p) { p.hit = 0; }
)HLSL";

// Phase 5 measurement, off by default. With --roundtrip, every shader this
// harness compiles is disassembled to text, reassembled, and re-signed before
// it ever reaches D3D12. The existing WARP against GTX 1070 comparison then
// answers the only question that matters about text rewriting: does a shader
// that has been through .ll and back still produce the same pixels?
static bool g_roundTrip = false;
static bool g_rtPoison  = false;   // deliberate corruption, to prove the check bites
static bool g_poisonNow = false;   // set per trial: poison only the lowered side

// Phase 5. With --lib <file>, the TraceRay side loads a pre-built DXIL library
// from disk instead of compiling HLSL, so a hand-lowered or rewriter-produced
// library can be checked against the same WARP ground truth as everything else.
static const char* g_libFile = nullptr;

// Phase 5. With --table, the UAV is reached through a DESCRIPTOR TABLE holding
// an array of 4 descriptors, and the shader writes through index 2. Only
// descriptor 2 points at the real output buffer; 0, 1 and 3 point at a decoy.
// So a lowering that dropped the array index would write to the decoy and the
// output would come back untouched, which the diff reports as a divergence.
// Without that asymmetry the test could not tell index 2 from index 0.
// --cs <file.hlsl> compiles that file for the RayQuery (ground truth) side
// instead of a built-in shader, so an independently written shader can be its
// own oracle: WARP runs the original, the 1070 runs what the rewriter made of
// it, and the two are diffed as usual.
static const char* g_csFile = nullptr;

// t1 is always in the root signature, bound to a small alpha mask. Shaders
// that do not declare it simply leave the root parameter unused, which is
// legal, so this costs the existing cases nothing.
static const UINT kMaskCount = 8;

// --multi builds a scene where InstanceIndex and PrimitiveIndex actually
// vary. Against the default one-triangle, one-instance scene every correct
// answer is 0, so a lowering that returned a constant 0 would pass.
static bool g_multi = false;

static bool g_table = false;
static const UINT kTableSize = 4;
static const UINT kTableSlot = 2;
static std::vector<uint8_t> ReadAll(const char* path) {
    std::vector<uint8_t> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) Throw("open --lib file", E_FAIL);
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    v.resize(n > 0 ? (size_t)n : 0);
    size_t got = v.empty() ? 0 : std::fread(v.data(), 1, v.size(), f);
    std::fclose(f);
    if (got != v.size()) Throw("read --lib file", E_FAIL);
    return v;
}

// --- DXC runtime compile ---------------------------------------------------
struct Dxc {
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcAssembler> assembler;
    ComPtr<IDxcValidator> validator;   // from dxil.dll, as in Phase 1
    ComPtr<IDxcCompiler> disassembler; // Disassemble() lives on IDxcCompiler
    void init() {
        HMODULE m = LoadLibraryW(L"dxcompiler.dll");
        if (!m) Throw("LoadLibrary(dxcompiler.dll)", HRESULT_FROM_WIN32(GetLastError()));
        auto create = (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance");
        if (!create) Throw("GetProcAddress(DxcCreateInstance)", E_FAIL);
        HR(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "create IDxcCompiler3");
        HR(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "create IDxcUtils");
        if (!g_roundTrip) return;
        HR(create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler)), "create IDxcAssembler");
        HR(create(CLSID_DxcCompiler, IID_PPV_ARGS(&disassembler)), "create IDxcCompiler");
        HMODULE d = LoadLibraryW(L"dxil.dll");
        if (!d) Throw("LoadLibrary(dxil.dll)", HRESULT_FROM_WIN32(GetLastError()));
        auto dcreate = (DxcCreateInstanceProc)GetProcAddress(d, "DxcCreateInstance");
        if (!dcreate) Throw("GetProcAddress(dxil DxcCreateInstance)", E_FAIL);
        HR(dcreate(CLSID_DxcValidator, IID_PPV_ARGS(&validator)), "create IDxcValidator");
    }

    // container -> .ll text -> container -> validated and re-signed container.
    ComPtr<IDxcBlob> roundTrip(IDxcBlob* in) {
        ComPtr<IDxcBlobEncoding> text;
        HR(disassembler->Disassemble(in, &text), "Disassemble");

        // Sensitivity check for the round trip itself. --rtpoison makes ONE
        // edit to the disassembly, flipping the ray direction from -Z to +Z so
        // every ray points away from the scene. If a poisoned run still reports
        // MATCH, then this harness is not actually sensitive to what the IR
        // says and a clean --roundtrip result would prove nothing. It is also
        // the project's first real DXIL text rewrite, however small.
        std::string ll(static_cast<const char*>(text->GetBufferPointer()),
                       text->GetBufferSize());
        if (g_rtPoison && g_poisonNow) {
            const std::string from = "float -1.000000e+00";
            const std::string to   = "float 1.000000e+00";
            size_t n = 0, at = 0;
            while ((at = ll.find(from, at)) != std::string::npos) {
                ll.replace(at, from.size(), to);
                at += to.size(); ++n;
            }
            std::printf("        [--rtpoison] flipped ray direction in %zu place(s)\n", n);
        }

        ComPtr<IDxcBlobEncoding> src;
        HR(utils->CreateBlob(ll.data(), (UINT32)ll.size(),
                             DXC_CP_ACP, &src), "CreateBlob(ll)");
        ComPtr<IDxcOperationResult> ares;
        HR(assembler->AssembleToContainer(src.Get(), &ares), "AssembleToContainer");
        HRESULT st = E_FAIL; ares->GetStatus(&st);
        if (FAILED(st)) Throw("reassembly", st);
        ComPtr<IDxcBlob> rebuilt;
        HR(ares->GetResult(&rebuilt), "assembler result");

        // InPlaceEdit signs the blob we hand it, the Phase 1 mechanism.
        ComPtr<IDxcOperationResult> vres;
        HR(validator->Validate(rebuilt.Get(), DxcValidatorFlags_InPlaceEdit, &vres),
           "Validate");
        vres->GetStatus(&st);
        if (FAILED(st)) {
            ComPtr<IDxcBlobEncoding> err;
            vres->GetErrorBuffer(&err);
            std::fprintf(stderr, "round-trip validation failed:\n%.*s\n",
                         err ? (int)err->GetBufferSize() : 0,
                         err ? (const char*)err->GetBufferPointer() : "");
            Throw("round-trip validation", st);
        }
        std::printf("        round-tripped through .ll: %zu -> %zu bytes\n",
                    (size_t)in->GetBufferSize(), (size_t)rebuilt->GetBufferSize());
        return rebuilt;
    }
    // Compile HLSL to a signed DXIL blob. entry may be empty for a lib target.
    ComPtr<IDxcBlob> compile(const char* src, const wchar_t* entry,
                             const wchar_t* target) {
        DxcBuffer buf{ src, std::strlen(src), DXC_CP_UTF8 };
        std::vector<const wchar_t*> args = { L"-T", target };
        if (entry && entry[0]) { args.push_back(L"-E"); args.push_back(entry); }
        ComPtr<IDxcResult> res;
        HR(compiler->Compile(&buf, args.data(), (UINT32)args.size(), nullptr,
                             IID_PPV_ARGS(&res)), "IDxcCompiler3::Compile");
        HRESULT status = E_FAIL; res->GetStatus(&status);
        if (FAILED(status)) {
            ComPtr<IDxcBlobUtf8> err;
            res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&err), nullptr);
            std::fprintf(stderr, "shader compile failed:\n%s\n",
                         err && err->GetStringLength() ? err->GetStringPointer() : "(no log)");
            Throw("shader compilation", status);
        }
        ComPtr<IDxcBlob> obj;
        HR(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr), "get DXIL object");
        if (g_roundTrip) return roundTrip(obj.Get());
        return obj;
    }
};

// --- small D3D12 helpers ---------------------------------------------------
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

// A device plus a single command queue/allocator/list, with a submit+wait flush.
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
    // Close, execute, wait for completion, then reset for the next batch.
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

// --- acceleration structures ----------------------------------------------
struct Scene {
    ComPtr<ID3D12Resource> vb;      // triangle vertices
    ComPtr<ID3D12Resource> blas;    // bottom-level AS
    ComPtr<ID3D12Resource> tlas;    // top-level AS
};

static ComPtr<ID3D12Resource> BuildAS(Gpu& g,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    g.device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
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
    g.flush();  // scratch stays alive until here
    return result;
}

// Two instances side by side, each a quad of TWO triangles. Across the
// screen: left half is instance 0 and right half instance 1, and within each
// quad the diagonal splits primitive 0 from primitive 1. So both indices vary
// per pixel and a constant answer cannot pass.
static Scene BuildSceneMulti(Gpu& g, bool opaque) {
    Scene s;
    const float qx = 0.4f, qy = 0.8f;
    const float verts[18] = {
        -qx, -qy, 0.0f,   qx, -qy, 0.0f,  -qx,  qy, 0.0f,   // primitive 0
         qx, -qy, 0.0f,   qx,  qy, 0.0f,  -qx,  qy, 0.0f,   // primitive 1
    };
    s.vb = CreateBuffer(g.device.Get(), sizeof(verts), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(s.vb->Map(0, &none, &p), "map vb"); std::memcpy(p, verts, sizeof(verts));
    s.vb->Unmap(0, nullptr);

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geo.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                       : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geo.Triangles.VertexBuffer.StartAddress = s.vb->GetGPUVirtualAddress();
    geo.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
    geo.Triangles.VertexCount = 6;
    geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geo.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bi{};
    bi.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bi.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bi.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bi.NumDescs = 1; bi.pGeometryDescs = &geo;
    s.blas = BuildAS(g, bi);

    D3D12_RAYTRACING_INSTANCE_DESC inst[2]{};
    for (int i = 0; i < 2; ++i) {
        inst[i].Transform[0][0] = inst[i].Transform[1][1] = inst[i].Transform[2][2] = 1.0f;
        inst[i].Transform[0][3] = (i == 0) ? -0.6f : 0.6f;   // side by side
        inst[i].InstanceMask = 0xFF;
        inst[i].AccelerationStructure = s.blas->GetGPUVirtualAddress();
    }
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

static Scene BuildScene(Gpu& g, bool opaque) {
    Scene s;
    // One triangle at z=0.
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

    // BLAS
    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geo.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                       : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geo.Triangles.VertexBuffer.StartAddress = s.vb->GetGPUVirtualAddress();
    geo.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
    geo.Triangles.VertexCount = 3;
    geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geo.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bi{};
    bi.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bi.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bi.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bi.NumDescs = 1; bi.pGeometryDescs = &geo;
    s.blas = BuildAS(g, bi);

    // TLAS with one identity instance.
    D3D12_RAYTRACING_INSTANCE_DESC inst{};
    inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;
    inst.InstanceMask = 0xFF;
    inst.AccelerationStructure = s.blas->GetGPUVirtualAddress();
    auto instBuf = CreateBuffer(g.device.Get(), sizeof(inst), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    HR(instBuf->Map(0, &none, &p), "map inst"); std::memcpy(p, &inst, sizeof(inst));
    instBuf->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
    ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    ti.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    ti.NumDescs = 1; ti.InstanceDescs = instBuf->GetGPUVirtualAddress();
    s.tlas = BuildAS(g, ti);
    return s;
}

// --- root signature shared by both methods: b0 CBV, t0 SRV(TLAS), u0 UAV(out)
static ComPtr<ID3D12RootSignature> MakeRootSig(ID3D12Device* dev) {
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
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1,
        &blob, &err);
    if (FAILED(hr)) {
        if (err) std::fprintf(stderr, "root sig: %.*s\n", (int)err->GetBufferSize(),
                              (const char*)err->GetBufferPointer());
        Throw("D3D12SerializeRootSignature", hr);
    }
    ComPtr<ID3D12RootSignature> rs;
    HR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&rs)), "CreateRootSignature");
    return rs;
}

// Bind b0/t0/u0 for a compute or DXR dispatch.
// Alternating 0 and 1, so a shader indexing it with a barycentric gets a
// stripe pattern. Only --cs shaders use it.
static ComPtr<ID3D12Resource> MakeAlphaMask(ID3D12Device* dev) {
    auto buf = CreateBuffer(dev, sizeof(float) * kMaskCount,
        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    float* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(buf->Map(0, &none, (void**)&p), "map alpha mask");
    for (UINT i = 0; i < kMaskCount; ++i) p[i] = (i & 1) ? 1.0f : 0.0f;
    buf->Unmap(0, nullptr);
    return buf;
}

static void BindRoots(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs,
        D3D12_GPU_VIRTUAL_ADDRESS cb, D3D12_GPU_VIRTUAL_ADDRESS tlas,
        D3D12_GPU_VIRTUAL_ADDRESS out, D3D12_GPU_VIRTUAL_ADDRESS mask = 0) {
    cl->SetComputeRootSignature(rs);
    cl->SetComputeRootConstantBufferView(0, cb);
    cl->SetComputeRootShaderResourceView(1, tlas);
    cl->SetComputeRootUnorderedAccessView(2, out);
    if (mask) cl->SetComputeRootShaderResourceView(3, mask);
}

// Root signature for --table: b0 and t0 stay root descriptors, the UAV array
// becomes a descriptor table of kTableSize entries starting at u0.
static ComPtr<ID3D12RootSignature> MakeRootSigTable(ID3D12Device* dev) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = kTableSize;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &range;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 4; rd.pParameters = params;
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1,
        &blob, &err);
    if (FAILED(hr)) {
        if (err) std::fprintf(stderr, "root sig: %.*s\n", (int)err->GetBufferSize(),
                              (const char*)err->GetBufferPointer());
        Throw("D3D12SerializeRootSignature(table)", hr);
    }
    ComPtr<ID3D12RootSignature> rs;
    HR(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&rs)), "CreateRootSignature(table)");
    return rs;
}

// ---------------------------------------------------------------------------
static void RunRayQuery(Gpu& g, Dxc& dxc, const Scene& s,
        ID3D12Resource* cb, ID3D12Resource* out, const char* hlsl) {
    auto rs = MakeRootSig(g.device.Get());
    std::string fromFile;
    if (g_csFile) {
        auto bytes = ReadAll(g_csFile);
        fromFile.assign((const char*)bytes.data(), bytes.size());
        hlsl = fromFile.c_str();
        std::printf("        compute shader from %s\n", g_csFile);
    }
    ComPtr<IDxcBlob> cs = dxc.compile(hlsl, L"main", L"cs_6_5");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    ComPtr<ID3D12PipelineState> pso;
    HR(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)),
       "CreateComputePipelineState");

    auto mask = MakeAlphaMask(g.device.Get());
    g.list->SetPipelineState(pso.Get());
    BindRoots(g.list.Get(), rs.Get(), cb->GetGPUVirtualAddress(),
              s.tlas->GetGPUVirtualAddress(), out->GetGPUVirtualAddress(),
              mask->GetGPUVirtualAddress());
    // Group count sized for the smallest thread group any of these shaders
    // uses. A 16x16 shader then gets more groups than it needs, which is safe
    // because every one of them bounds checks; too FEW groups would not be.
    g.list->Dispatch((kWidth + 7) / 8, (kHeight + 7) / 8, 1);
    UavBarrier(g.list.Get(), out);
    g.flush();
}

// A shader table with one record: just a 32-byte identifier, table 64-aligned.
static ComPtr<ID3D12Resource> MakeSBT(ID3D12Device* dev, const void* ident) {
    const UINT idSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
    UINT64 size = (idSize + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT - 1)
                & ~(UINT64)(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT - 1);
    auto buf = CreateBuffer(dev, size, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(buf->Map(0, &none, &p), "map SBT");
    std::memcpy(p, ident, idSize);
    buf->Unmap(0, nullptr);
    return buf;
}

static void RunTraceRay(Gpu& g, Dxc& dxc, const Scene& s,
        ID3D12Resource* cb, ID3D12Resource* out, const char* hlsl, bool anyHit) {
    auto rs = g_table ? MakeRootSigTable(g.device.Get())
                      : MakeRootSig(g.device.Get());

    // The descriptor table and its decoy, built only for --table.
    auto mask = MakeAlphaMask(g.device.Get());
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> decoy;
    if (g_table) {
        const UINT64 bytes = (UINT64)kWidth * kHeight * sizeof(Result);
        decoy = CreateBuffer(g.device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kTableSize;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)),
           "CreateDescriptorHeap");

        const UINT stride = g.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto base = heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kTableSize; ++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.Buffer.NumElements = kWidth * kHeight;
            ud.Buffer.StructureByteStride = sizeof(Result);
            D3D12_CPU_DESCRIPTOR_HANDLE h{ base.ptr + (SIZE_T)i * stride };
            // Only the slot the shader indexes gets the real buffer.
            g.device->CreateUnorderedAccessView(
                i == kTableSlot ? out : decoy.Get(), nullptr, &ud, h);
        }
    }

    // --- state object subobjects ---
    std::vector<D3D12_STATE_SUBOBJECT> subs;

    ComPtr<IDxcBlob> lib;
    std::vector<uint8_t> fileLib;
    D3D12_DXIL_LIBRARY_DESC libDesc{};
    if (g_libFile) {
        fileLib = ReadAll(g_libFile);
        std::printf("        library loaded from %s (%zu bytes)\n",
                    g_libFile, fileLib.size());
        libDesc.DXILLibrary.pShaderBytecode = fileLib.data();
        libDesc.DXILLibrary.BytecodeLength = fileLib.size();
    } else {
        lib = dxc.compile(hlsl, L"", L"lib_6_3");
        libDesc.DXILLibrary.pShaderBytecode = lib->GetBufferPointer();
        libDesc.DXILLibrary.BytecodeLength = lib->GetBufferSize();
    }
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc });

    D3D12_HIT_GROUP_DESC hg{};
    hg.HitGroupExport = L"HitGroup";
    hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg.ClosestHitShaderImport = L"ClosestHit";
    if (anyHit) hg.AnyHitShaderImport = L"AnyHit";
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg });

    D3D12_RAYTRACING_SHADER_CONFIG sc{};
    // Must be at least the payload the rewriter emits, PAYLOAD_BYTES in
    // phase5/rewriter/lower.py, currently 84: t, bary, hit, the instance,
    // primitive and instance ID, the hit kind, and the 3x4 world-to-object
    // matrix. It is a MAXIMUM, so declaring
    // 28 costs the 16-byte hand-written shaders nothing. Getting this wrong
    // shows up as CreateStateObject returning E_INVALIDARG, which is a real
    // coupling: the payload size is part of the state object contract, not a
    // free choice for whatever generates the shaders.
    sc.MaxPayloadSizeInBytes = 84;
    sc.MaxAttributeSizeInBytes = 8;     // float2 barycentrics
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });

    ID3D12RootSignature* rsPtr = rs.Get();
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &rsPtr });

    D3D12_RAYTRACING_PIPELINE_CONFIG pc{}; pc.MaxTraceRecursionDepth = 1;
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });

    D3D12_STATE_OBJECT_DESC soDesc{};
    soDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    soDesc.NumSubobjects = (UINT)subs.size();
    soDesc.pSubobjects = subs.data();
    ComPtr<ID3D12StateObject> so;
    HR(g.device->CreateStateObject(&soDesc, IID_PPV_ARGS(&so)), "CreateStateObject");
    ComPtr<ID3D12StateObjectProperties> props;
    HR(so.As(&props), "StateObjectProperties");

    auto sbtRayGen = MakeSBT(g.device.Get(), props->GetShaderIdentifier(L"RayGen"));
    auto sbtMiss   = MakeSBT(g.device.Get(), props->GetShaderIdentifier(L"Miss"));
    auto sbtHit    = MakeSBT(g.device.Get(), props->GetShaderIdentifier(L"HitGroup"));

    D3D12_DISPATCH_RAYS_DESC dr{};
    const UINT idSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dr.RayGenerationShaderRecord.StartAddress = sbtRayGen->GetGPUVirtualAddress();
    dr.RayGenerationShaderRecord.SizeInBytes = idSize;
    dr.MissShaderTable.StartAddress = sbtMiss->GetGPUVirtualAddress();
    dr.MissShaderTable.SizeInBytes = idSize;
    dr.MissShaderTable.StrideInBytes = idSize;
    dr.HitGroupTable.StartAddress = sbtHit->GetGPUVirtualAddress();
    dr.HitGroupTable.SizeInBytes = idSize;
    dr.HitGroupTable.StrideInBytes = idSize;
    dr.Width = kWidth; dr.Height = kHeight; dr.Depth = 1;

    g.list->SetPipelineState1(so.Get());
    if (g_table) {
        ID3D12DescriptorHeap* heaps[] = { heap.Get() };
        g.list->SetDescriptorHeaps(1, heaps);
        g.list->SetComputeRootSignature(rs.Get());
        g.list->SetComputeRootConstantBufferView(0, cb->GetGPUVirtualAddress());
        g.list->SetComputeRootShaderResourceView(1, s.tlas->GetGPUVirtualAddress());
        g.list->SetComputeRootDescriptorTable(
            2, heap->GetGPUDescriptorHandleForHeapStart());
        g.list->SetComputeRootShaderResourceView(3, mask->GetGPUVirtualAddress());
    } else {
        BindRoots(g.list.Get(), rs.Get(), cb->GetGPUVirtualAddress(),
                  s.tlas->GetGPUVirtualAddress(), out->GetGPUVirtualAddress(),
                  mask->GetGPUVirtualAddress());
    }
    g.list->DispatchRays(&dr);
    UavBarrier(g.list.Get(), out);
    g.flush();
    // SBT/state object stay alive until flush completes (scope).
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
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;  // skip WARP
        return adapter;
    }
    throw std::runtime_error("no hardware adapter found");
}

static void WriteFile(const char* path, const std::vector<Result>& data) {
    FileHeader h{ kMagic, kWidth, kHeight, (uint32_t)sizeof(Result) };
    FILE* f = std::fopen(path, "wb");
    if (!f) throw std::runtime_error(std::string("cannot open ") + path);
    std::fwrite(&h, sizeof(h), 1, f);
    std::fwrite(data.data(), sizeof(Result), data.size(), f);
    std::fclose(f);
}

// Run one adapter+method trial and dump the per-ray results to `outPath`.
static void RunTrial(Adapter adapter, Method method, Pattern pattern, const char* outPath) {
    const char* an = (adapter == Adapter::Warp) ? "WARP" : "hardware";
    const char* mn = (method == Method::RayQuery) ? "RayQuery(compute)" : "TraceRay(DXR1.0)";
    const char* pn = (pattern == Pattern::Opaque) ? "opaque" : "alpha";
    std::printf("[trial] %s %s on %s -> %s\n", pn, mn, an, outPath);

    // Poison only the LOWERED side. Corrupting both would leave them agreeing
    // with each other, so the diff would still report MATCH and would prove
    // nothing about whether it can see an IR change at all.
    g_poisonNow = (method == Method::TraceRay);

    ComPtr<IDXGIFactory6> factory;
    UINT flags = 0;
#ifdef _DEBUG
    { ComPtr<ID3D12Debug> dbg; if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer(); }
    flags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    HR(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    auto adapt = PickAdapter(factory.Get(), adapter);

    ComPtr<ID3D12Device5> device;
    HR(D3D12CreateDevice(adapt.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
       "D3D12CreateDevice");

    // Feature gates.
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    HR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)),
       "CheckFeatureSupport(OPTIONS5)");
    std::printf("        RaytracingTier = 0x%x\n", o5.RaytracingTier);
    if (method == Method::RayQuery &&
        o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
        throw std::runtime_error("RayQuery needs Tier 1.1; this adapter lacks it");
    if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
        throw std::runtime_error("adapter reports no raytracing support");
    if (method == Method::RayQuery) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_5 };
        HR(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)),
           "CheckFeatureSupport(SHADER_MODEL)");
        if (sm.HighestShaderModel < D3D_SHADER_MODEL_6_5)
            throw std::runtime_error("RayQuery needs Shader Model 6.5");
    }

    Gpu g; g.init(device.Get());
    Dxc dxc; dxc.init();
    Scene scene = g_multi ? BuildSceneMulti(g, pattern == Pattern::Opaque)
                          : BuildScene(g, pattern == Pattern::Opaque);

    // Constant buffer.
    SceneCB cbData{ kWidth, kHeight, kHalfExtent, kCamZ, kTMin, kTMax, 0, 0 };
    UINT64 cbSize = (sizeof(SceneCB) + 255) & ~255ull;
    auto cb = CreateBuffer(device.Get(), cbSize, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(cb->Map(0, &none, &p), "map cb"); std::memcpy(p, &cbData, sizeof(cbData));
    cb->Unmap(0, nullptr);

    // Output UAV buffer + readback.
    UINT64 outSize = (UINT64)kWidth * kHeight * sizeof(Result);
    auto out = CreateBuffer(device.Get(), outSize, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = CreateBuffer(device.Get(), outSize, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);

    if (method == Method::RayQuery) {
        const char* hlsl = (pattern == Pattern::Opaque) ? kRayQueryHLSL : kRayQueryAlphaHLSL;
        RunRayQuery(g, dxc, scene, cb.Get(), out.Get(), hlsl);
    } else {
        const char* hlsl = (pattern == Pattern::Opaque) ? kTraceRayHLSL : kTraceRayAlphaHLSL;
        RunTraceRay(g, dxc, scene, cb.Get(), out.Get(), hlsl, pattern == Pattern::Alpha);
    }

    // Copy out -> readback.
    Transition(g.list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyResource(readback.Get(), out.Get());
    g.flush();

    std::vector<Result> results(kWidth * kHeight);
    void* rp = nullptr; D3D12_RANGE full{ 0, (SIZE_T)outSize };
    HR(readback->Map(0, &full, &rp), "map readback");
    std::memcpy(results.data(), rp, outSize);
    D3D12_RANGE nowrite{ 0, 0 }; readback->Unmap(0, &nowrite);

    uint32_t hits = 0; for (auto& r : results) hits += r.hit ? 1 : 0;
    std::printf("        %u rays, %u hits\n", kWidth * kHeight, hits);
    WriteFile(outPath, results);
}

// --- diff ------------------------------------------------------------------
static std::vector<Result> ReadFile(const char* path, uint32_t& w, uint32_t& h) {
    FILE* f = std::fopen(path, "rb");
    if (!f) throw std::runtime_error(std::string("cannot open ") + path);
    FileHeader hd{}; std::fread(&hd, sizeof(hd), 1, f);
    if (hd.magic != kMagic || hd.rec != sizeof(Result)) {
        std::fclose(f); throw std::runtime_error(std::string("bad file: ") + path);
    }
    w = hd.width; h = hd.height;
    std::vector<Result> v((size_t)w * h);
    std::fread(v.data(), sizeof(Result), v.size(), f);
    std::fclose(f);
    return v;
}

static int Diff(const char* pa, const char* pb) {
    uint32_t wa, ha, wb, hb;
    auto a = ReadFile(pa, wa, ha);
    auto b = ReadFile(pb, wb, hb);
    if (wa != wb || ha != hb) { std::printf("dimension mismatch\n"); return 4; }

    const float tol = 1e-3f;
    uint32_t hitMismatch = 0, valMismatch = 0;
    float maxT = 0, maxB = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].hit != b[i].hit) { ++hitMismatch; continue; }
        if (!a[i].hit) continue;  // both miss: nothing else to compare
        float dt = std::fabs(a[i].t - b[i].t);
        float dbx = std::fabs(a[i].bx - b[i].bx);
        float dby = std::fabs(a[i].by - b[i].by);
        maxT = dt > maxT ? dt : maxT;
        float db = dbx > dby ? dbx : dby;
        maxB = db > maxB ? db : maxB;
        if (dt > tol || db > tol) ++valMismatch;
    }
    std::printf("\n==== DIFF %s vs %s ====\n", pa, pb);
    std::printf("  rays               : %zu\n", a.size());
    std::printf("  hit/miss mismatches: %u\n", hitMismatch);
    std::printf("  value  mismatches  : %u (tol %.4f)\n", valMismatch, tol);
    std::printf("  max |dt|           : %.6f\n", maxT);
    std::printf("  max |dbary|        : %.6f\n", maxB);
    bool ok = (hitMismatch == 0 && valMismatch == 0);
    std::printf("  RESULT: %s\n", ok ? "MATCH (lowering sound)" : "DIVERGE");
    return ok ? 0 : 4;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    try {
        // Pull --roundtrip out of argv so the positional arguments below are
        // unaffected by it.
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--multi") == 0) {
                g_multi = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--cs") == 0 && i + 1 < argc) {
                g_csFile = argv[i + 1];
                for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
                argc -= 2; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--table") == 0) {
                g_table = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
                g_libFile = argv[i + 1];
                for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
                argc -= 2; --i;
                continue;
            }
            const bool poison = std::strcmp(argv[i], "--rtpoison") == 0;
            if (!poison && std::strcmp(argv[i], "--roundtrip") != 0) continue;
            g_roundTrip = true;
            if (poison) g_rtPoison = true;
            for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
            --argc; --i;
        }
        if (g_roundTrip)
            std::printf("[--roundtrip] every shader goes through .ll and back "
                        "before D3D12 sees it\n");

        if (argc >= 4 && std::strcmp(argv[1], "diff") == 0)
            return Diff(argv[2], argv[3]);

        // Single trial: raytest.exe <warp|hw> <rayquery|traceray> <opaque|alpha> <out>
        if (argc >= 5) {
            Adapter a = std::strcmp(argv[1], "warp") == 0 ? Adapter::Warp : Adapter::Hardware;
            Method  m = std::strcmp(argv[2], "rayquery") == 0 ? Method::RayQuery : Method::TraceRay;
            Pattern p = std::strcmp(argv[3], "alpha") == 0 ? Pattern::Alpha : Pattern::Opaque;
            RunTrial(a, m, p, argv[4]);
            return 0;
        }

        // Default: run both patterns, each ground-truth (WARP RayQuery) vs
        // lowered (hardware TraceRay), and diff.
        int rc = 0;
        for (Pattern p : { Pattern::Opaque, Pattern::Alpha }) {
            const char* tag = (p == Pattern::Opaque) ? "opaque" : "alpha";
            std::string fa = std::string(tag) + "_a.bin";
            std::string fb = std::string(tag) + "_b.bin";
            std::printf("\n### pattern: %s ###\n", tag);
            RunTrial(Adapter::Warp,     Method::RayQuery, p, fa.c_str());
            RunTrial(Adapter::Hardware, Method::TraceRay, p, fb.c_str());
            rc |= Diff(fa.c_str(), fb.c_str());
        }
        std::printf("\n==== OVERALL: %s ====\n", rc == 0 ? "ALL MATCH" : "DIVERGENCE");
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FATAL] %s\n", e.what());
        return 1;
    }
}
