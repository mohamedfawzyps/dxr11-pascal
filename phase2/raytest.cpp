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
#include <algorithm>
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
// With --contrib the two instances get different hit group contributions,
// which no single-record shader table can serve. Used to show the shim
// refuses that scene instead of drawing it wrong.
static bool g_contrib = false;

// --proc builds a scene of PROCEDURAL geometry, one AABB, and switches the hit
// group to D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE with the generated
// intersection shader. A BLAS carries one geometry type, so procedural and
// triangle geometry can never share one.
static bool g_proc = false;
// One triangle instance and one procedural instance under one TLAS.
static bool g_mixed = false;
// The lowered library commits BOTH kinds, so it exports two hit groups and the
// hit table needs a record for each. Record 0 is triangles and record 1 is
// procedural, which is what --mixed --contrib builds.
static bool g_both = false;

static bool g_table = false;
// --indirect: the RayQuery compute shader is dispatched through ExecuteIndirect
// with a DISPATCH signature, which is how Unreal issues most of its Lumen and
// MegaLights inline passes. The arguments are written by the GPU, left in a
// COMBINED read state (INDIRECT_ARGUMENT | NON_PIXEL_SHADER_RESOURCE), and sit
// at a nonzero offset among decoy values, as a sub-allocation would. So a shim
// that ignores the dispatch draws nothing, one that reads the wrong offset
// draws a decoy-sized corner, and one that transitions the buffer from the
// wrong state is caught by the debug layer. --indirectup puts the same
// arguments in an upload buffer, the CPU-visible path.
static bool g_indirect = false;
// --rebuild, with --multi --contrib: the top-level structure is first built
// with EVERY contribution 0 and dispatched against, then rebuilt IN PLACE, the
// same instance buffer and the same destination address, with contributions
// 0 and 1, and dispatched again. Only the second result is compared. This is
// what an engine does when a level loads, and a shim that reads the instance
// data once per address keeps the first answer: a one-record table, and every
// hit on the second instance lands past its end.
static bool g_rebuild = false;
// --churn N, with --geom: after the real dispatch, in the SAME command list and
// before anything is submitted, rebuild the top-level structure N times, each
// with a different contribution, and dispatch into a decoy after each. Every
// layout gets its own shader table, so the real dispatch's table is pushed out
// of the shim's cache while the list that reads it has not even run. If the
// shim reused an evicted table before the GPU was done with it, the real
// dispatch would read another layout's pairs. Only the real output is
// compared, and WARP runs the same churn, so it stays the ground truth.
static int g_churn = 0;
// --churnflush, with --churn: submit and wait after EVERY layout instead, so
// the evicted tables become idle and the shim's reuse and release path runs.
static bool g_churnFlush = false;
// --move, with --geom --contrib: dispatch against the scene, then build it
// again at a NEW address with the contribution one higher, as Unreal does
// when its top-level structure outgrows its buffer, rebuild it there three
// more times, and dispatch against the new one. Only the second result is
// compared. The old structure is never built again, so it must stop counting
// as live; while it does count, the two disagree about records and the shim
// refuses, which draws nothing.
static bool g_move = false;
// --decoy, with --geom: a SECOND live top-level structure whose layout puts
// different (contribution, geometry) pairs on the records the real one uses.
// Judged over every live structure, the lowered dispatch is refused; the shim
// has to know which one the dispatch traces. --bindless does the same with
// the scene at descriptor heap slot 3 (ResourceDescriptorHeap[i], i in the
// cbuffer), the decoy in the slots around it and in the root SRV at t0.
static bool g_decoy = false;
static bool g_bindless = false;
// --append: bind a RWStructuredBuffer<uint> WITH A UAV COUNTER at u1 through a
// descriptor table, root parameter 4, for a shader whose Proceed loop appends
// a record per candidate. After the dispatch the counter and the records are
// read back and written, SORTED, to <out>.append: the order records land in is
// traversal order, which is implementation-defined, so only the multiset is
// comparable between WARP and the 1070. Direct dispatch only.
static bool g_append = false;
// --prefill N: fill the output buffer with a pattern seeded by N before the
// dispatch, on both sides, so a shader that READS its output slot before the
// trace (Unreal's MegaLights does) reads data rather than zeros. Different
// seeds on the two sides must diverge; see the preload case.
static unsigned g_prefill = 0;
static std::string g_outPath;
static const UINT kLogCount = 65536;
static bool g_indirectUp = false;
// Create the compute PSO through CreatePipelineState, the pipeline STREAM
// form, instead of CreateComputePipelineState. Unreal uses the stream form
// for everything, and the shim's substitution used to cover only the struct
// form, so every RayQuery shader in a real engine went straight to the
// driver unexamined. Same shader, same scene, different D3D12 entry point.
static bool g_stream = false;
// One bottom-level structure holding FOUR geometries, so GeometryIndex varies
// across the image instead of being 0 everywhere.
//
// Every other scene here has one geometry per structure, which means every
// correct answer for GeometryIndex is 0 and a lowering that returned a
// constant would pass. That is the whole reason this scene exists.
static bool g_geom = false;
// --gpuinst: every top-level build takes its instance descriptions from GPU
// memory, copied there on the GPU just before the build, as Unreal writes
// them. The shim cannot read those at record time: it reads them back, a
// submission late. With --rebuild that makes what it read the OLDER build.
static bool g_gpuinst = false;
// --empty, with --geom: the top-level structure holds NO instances, as
// Unreal's does on the first frame of a level. Every ray misses. Use with
// --prefill so a dispatch that is not drawn cannot pass for one that missed.
static bool g_empty = false;
// --sameecl, with --rebuild: the in-place rebuild is recorded into a SECOND
// command list, submitted in the same ExecuteCommandLists call as the
// dispatch, just before it. Engines submit many lists at once.
static bool g_sameEcl = false;
// --blasreuse, with --geom, direct dispatch: AFTER the dispatch is recorded,
// the bottom-level structure is rebuilt at the SAME address with one geometry
// instead of four, in a second list submitted after the dispatch's, in the
// same ExecuteCommandLists call. The dispatch runs first, so it traces four
// geometries. A shim that takes a structure's geometry count from the latest
// build RECORDED, rather than the one before the dispatch, gets one. Unreal
// streams geometry in and out, reusing addresses, while it records the next
// frame ahead of the GPU.
static bool g_blasReuse = false;
// With --geom: the structures the dispatch uses are COPIES, made by
// CopyRaytracingAccelerationStructure, which the shim never sees built.
// --blasclone: the instance points at a CLONE of the bottom-level structure,
// as Unreal's compacted ones are COMPACT copies. --tlasclone: the dispatch
// traces a CLONE of the top-level one. --deserialize: the bottom-level one is
// serialized and deserialized, as Unreal loads offline structures; its
// geometry is driver-opaque, so the shim must refuse, not guess.
static bool g_blasClone = false, g_tlasClone = false, g_deserialize = false;
// With --geom: a second instance whose AccelerationStructure is NULL, as
// Unreal writes a culled instance. The spec calls it legal but inactive,
// discarded at build: it reaches no record and nothing is refused for it.
static bool g_nullInst = false;
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
static std::vector<ComPtr<ID3D12Resource>>& UploadRegistry();
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
    if (g_gpuinst && heap == D3D12_HEAP_TYPE_UPLOAD) UploadRegistry().push_back(r);
    return r;
}

// --gpuinst: every upload buffer, so a top-level build's instance address can
// be traced back to the buffer it lives in, and the GPU copies made of them.
static std::vector<ComPtr<ID3D12Resource>> g_uploads, g_gpuInstCopies;
static std::vector<ComPtr<ID3D12Resource>>& UploadRegistry() { return g_uploads; }

// Records one acceleration structure build. With --gpuinst a top-level one
// first has its instance descriptions copied into GPU memory, on the GPU.
static void RecordBuild(ID3D12Device* dev, ID3D12GraphicsCommandList4* list,
                        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc) {
    const auto& in = desc.Inputs;
    if (g_gpuinst && in.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL &&
        in.NumDescs) {
        const UINT64 bytes = (UINT64)in.NumDescs * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        ID3D12Resource* src = nullptr;
        for (auto& u : g_uploads) {
            const auto a = u->GetGPUVirtualAddress();
            if (in.InstanceDescs >= a && in.InstanceDescs + bytes <= a + u->GetDesc().Width) src = u.Get();
        }
        if (!src) throw std::runtime_error("--gpuinst: instance buffer not found");
        auto gb = CreateBuffer(dev, bytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(gb.Get(), 0, src, in.InstanceDescs - src->GetGPUVirtualAddress(), bytes);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = gb.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &b);
        desc.Inputs.InstanceDescs = gb->GetGPUVirtualAddress();
        g_gpuInstCopies.push_back(gb);
    }
    list->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
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
    // --sameecl: a second list, opened on demand and submitted just before
    // `list` in the same ExecuteCommandLists call.
    ComPtr<ID3D12CommandAllocator> preAlloc;
    ComPtr<ID3D12GraphicsCommandList4> pre;
    bool preOpen = false;
    ID3D12GraphicsCommandList4* Pre() {
        if (!pre) {
            HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&preAlloc)), "CreateCommandAllocator pre");
            HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, preAlloc.Get(),
                nullptr, IID_PPV_ARGS(&pre)), "CreateCommandList pre");
        } else if (!preOpen) {
            HR(preAlloc->Reset(), "pre allocator Reset");
            HR(pre->Reset(preAlloc.Get(), nullptr), "pre Reset");
        }
        preOpen = true;
        return pre.Get();
    }
    // --blasreuse: a list submitted just AFTER `list`, in the same call.
    ComPtr<ID3D12CommandAllocator> postAlloc;
    ComPtr<ID3D12GraphicsCommandList4> post;
    bool postOpen = false;
    ID3D12GraphicsCommandList4* Post() {
        if (!post) {
            HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&postAlloc)), "CreateCommandAllocator post");
            HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, postAlloc.Get(),
                nullptr, IID_PPV_ARGS(&post)), "CreateCommandList post");
        } else if (!postOpen) {
            HR(postAlloc->Reset(), "post allocator Reset");
            HR(post->Reset(postAlloc.Get(), nullptr), "post Reset");
        }
        postOpen = true;
        return post.Get();
    }
    void flush() {
        HR(list->Close(), "cmdlist Close");
        ID3D12CommandList* lists[3];
        UINT n = 0;
        if (preOpen) { HR(pre->Close(), "pre Close"); lists[n++] = pre.Get(); }
        lists[n++] = list.Get();
        if (postOpen) { HR(post->Close(), "post Close"); lists[n++] = post.Get(); }
        queue->ExecuteCommandLists(n, lists);
        preOpen = postOpen = false;
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
    // Only the mixed scene uses these: a BLAS carries ONE geometry type, so
    // triangles and procedural primitives need one structure each.
    ComPtr<ID3D12Resource> vb2;
    ComPtr<ID3D12Resource> blas2;
    // Only --rebuild keeps these: the instance buffer the TLAS was built from.
    ComPtr<ID3D12Resource> inst;
    UINT instCount = 0;
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
    RecordBuild(g.device.Get(), g.list.Get(), desc);
    UavBarrier(g.list.Get(), result.Get());
    g.flush();  // scratch stays alive until here
    return result;
}

// Two instances side by side, each a quad of TWO triangles. Across the
// screen: left half is instance 0 and right half instance 1, and within each
// quad the diagonal splits primitive 0 from primitive 1. So both indices vary
// per pixel and a constant answer cannot pass.
// One AABB at the origin, enclosing the box the shader intersects itself.
// The AABB only has to BOUND the primitive; the actual intersection is the
// shader's job, which is the whole point of procedural geometry.
static Scene BuildSceneProc(Gpu& g, bool opaque) {
    Scene s;
    const D3D12_RAYTRACING_AABB aabb = { -0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f };
    s.vb = CreateBuffer(g.device.Get(), sizeof(aabb), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(s.vb->Map(0, &none, &p), "map aabb"); std::memcpy(p, &aabb, sizeof(aabb));
    s.vb->Unmap(0, nullptr);

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    // Non-opaque, so traversal yields the candidate to the shader rather than
    // accepting it outright.
    geo.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                       : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geo.AABBs.AABBCount = 1;
    geo.AABBs.AABBs.StartAddress = s.vb->GetGPUVirtualAddress();
    geo.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bi{};
    bi.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bi.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bi.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bi.NumDescs = 1; bi.pGeometryDescs = &geo;
    s.blas = BuildAS(g, bi);

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

// A triangle instance and a procedural instance under ONE top-level
// structure, which is the shape the shim could not previously build a table
// for. The AABB sits to the left of the triangle and inside the ray grid, so
// both geometries are actually traversed and the two contributions can be told
// apart in the output.
static Scene BuildSceneMixed(Gpu& g, bool opaque) {
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

    // The SAME unit box the procedural-only scene uses, so the existing
    // procedural test shader works on this scene unchanged. It overlaps the
    // triangle in screen space, which is the point: both geometries are
    // traversed for the same rays, and which one commits is decided by the
    // shader rather than by the scene.
    const D3D12_RAYTRACING_AABB aabb = { -0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f };
    s.vb2 = CreateBuffer(g.device.Get(), sizeof(aabb), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    HR(s.vb2->Map(0, &none, &p), "map aabb"); std::memcpy(p, &aabb, sizeof(aabb));
    s.vb2->Unmap(0, nullptr);

    D3D12_RAYTRACING_GEOMETRY_DESC box{};
    box.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    box.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                       : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    box.AABBs.AABBCount = 1;
    box.AABBs.AABBs.StartAddress = s.vb2->GetGPUVirtualAddress();
    box.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);
    bi.pGeometryDescs = &box;
    s.blas2 = BuildAS(g, bi);

    D3D12_RAYTRACING_INSTANCE_DESC inst[2]{};
    for (int i = 0; i < 2; ++i) {
        inst[i].Transform[0][0] = inst[i].Transform[1][1] = inst[i].Transform[2][2] = 1.0f;
        inst[i].InstanceMask = 0xFF;
        // With --contrib the two geometries land on DIFFERENT hit group
        // records, which is what a real engine does when it needs one shader
        // per kind. Without it they share record 0.
        if (g_contrib) inst[i].InstanceContributionToHitGroupIndex = (UINT)i;
    }
    inst[0].AccelerationStructure = s.blas->GetGPUVirtualAddress();
    inst[1].AccelerationStructure = s.blas2->GetGPUVirtualAddress();

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

// One BLAS, four geometries, one quad each, one per quadrant of the view.
//
// GeometryIndex is then a function of where the ray lands: 0 bottom-left,
// 1 bottom-right, 2 top-left, 3 top-right. A shader reading it produces a
// four-way pattern, and any lowering that loses the index produces a flat one.
//
// The quads stop short of the axes so no ray lands exactly on a shared edge,
// where which geometry is hit would be a tie and the oracle could legitimately
// disagree with the hardware.
static Scene BuildSceneGeom(Gpu& g, bool opaque) {
    Scene s;
    const float lo = 0.1f, hi = 0.9f;
    struct Quad { float x0, y0, x1, y1; };
    const Quad quads[4] = {
        { -hi, -hi, -lo, -lo },   // geometry 0
        {  lo, -hi,  hi, -lo },   // geometry 1
        { -hi,  lo, -lo,  hi },   // geometry 2
        {  lo,  lo,  hi,  hi },   // geometry 3
    };
    float verts[4 * 6 * 3];
    for (int q = 0; q < 4; ++q) {
        const Quad& r = quads[q];
        const float tri[18] = {
            r.x0, r.y0, 0.0f,  r.x1, r.y0, 0.0f,  r.x0, r.y1, 0.0f,
            r.x1, r.y0, 0.0f,  r.x1, r.y1, 0.0f,  r.x0, r.y1, 0.0f,
        };
        std::memcpy(verts + q * 18, tri, sizeof(tri));
    }
    s.vb = CreateBuffer(g.device.Get(), sizeof(verts), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(s.vb->Map(0, &none, &p), "map vb"); std::memcpy(p, verts, sizeof(verts));
    s.vb->Unmap(0, nullptr);

    D3D12_RAYTRACING_GEOMETRY_DESC geo[4]{};
    for (int q = 0; q < 4; ++q) {
        geo[q].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geo[q].Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                              : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geo[q].Triangles.VertexBuffer.StartAddress =
            s.vb->GetGPUVirtualAddress() + q * 6 * sizeof(float) * 3;
        geo[q].Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
        geo[q].Triangles.VertexCount = 6;
        geo[q].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geo[q].Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bi{};
    bi.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bi.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bi.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bi.NumDescs = 4; bi.pGeometryDescs = geo;
    s.blas = BuildAS(g, bi);
    if (g_blasClone || g_deserialize) {
        const UINT64 w = s.blas->GetDesc().Width;
        auto copy = CreateBuffer(g.device.Get(), w, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (g_blasClone) {
            g.list->CopyRaytracingAccelerationStructure(copy->GetGPUVirtualAddress(),
                s.blas->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
        } else {
            // The serialized size, from the postbuild info.
            auto info = CreateBuffer(g.device.Get(), 256, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pd{};
            pd.DestBuffer = info->GetGPUVirtualAddress();
            pd.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION;
            const D3D12_GPU_VIRTUAL_ADDRESS src = s.blas->GetGPUVirtualAddress();
            g.list->EmitRaytracingAccelerationStructurePostbuildInfo(&pd, 1, &src);
            Transition(g.list.Get(), info.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            auto rb = CreateBuffer(g.device.Get(), 256, D3D12_HEAP_TYPE_READBACK,
                D3D12_RESOURCE_STATE_COPY_DEST);
            g.list->CopyBufferRegion(rb.Get(), 0, info.Get(), 0, 256);
            g.flush();
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC sd{};
            void* m = nullptr;
            HR(rb->Map(0, nullptr, &m), "map serialization info");
            std::memcpy(&sd, m, sizeof(sd));
            rb->Unmap(0, nullptr);
            if (!sd.SerializedSizeInBytes) throw std::runtime_error("serialized size 0");
            auto ser = CreateBuffer(g.device.Get(), sd.SerializedSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            g.list->CopyRaytracingAccelerationStructure(ser->GetGPUVirtualAddress(), src,
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
            Transition(g.list.Get(), ser.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g.list->CopyRaytracingAccelerationStructure(copy->GetGPUVirtualAddress(),
                ser->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
            g_gpuInstCopies.push_back(ser);
        }
        UavBarrier(g.list.Get(), copy.Get());
        g.flush();
        g_gpuInstCopies.push_back(s.blas);
        s.blas = copy;
    }

    // One instance. With --contrib it carries a nonzero contribution as well,
    // so the record index becomes contribution + geometry index and BOTH
    // numbers have to be right rather than just one of them.
    D3D12_RAYTRACING_INSTANCE_DESC inst{};
    inst.Transform[0][0] = inst.Transform[1][1] = inst.Transform[2][2] = 1.0f;
    inst.InstanceMask = 0xFF;
    if (g_contrib) inst.InstanceContributionToHitGroupIndex = 2;
    inst.AccelerationStructure = s.blas->GetGPUVirtualAddress();
    D3D12_RAYTRACING_INSTANCE_DESC insts[2] = { inst, inst };
    insts[1].AccelerationStructure = 0;
    insts[1].InstanceContributionToHitGroupIndex = 7;
    const UINT numInst = g_nullInst ? 2 : 1;
    auto instBuf = CreateBuffer(g.device.Get(), sizeof(insts), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    HR(instBuf->Map(0, &none, &p), "map inst"); std::memcpy(p, insts, sizeof(insts));
    instBuf->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
    ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    ti.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    ti.NumDescs = g_empty ? 0 : numInst; ti.InstanceDescs = instBuf->GetGPUVirtualAddress();
    s.tlas = BuildAS(g, ti);
    if (g_tlasClone) {
        auto copy = CreateBuffer(g.device.Get(), s.tlas->GetDesc().Width, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        g.list->CopyRaytracingAccelerationStructure(copy->GetGPUVirtualAddress(),
            s.tlas->GetGPUVirtualAddress(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
        UavBarrier(g.list.Get(), copy.Get());
        g.flush();
        g_gpuInstCopies.push_back(s.tlas);
        s.tlas = copy;
    }
    if (g_churn || g_move || g_decoy || g_bindless || g_rebuild) { s.inst = instBuf; s.instCount = 1; }
    return s;
}

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
        // --rebuild starts from all zeros; the real contributions come later.
        if (g_contrib && !g_rebuild) inst[i].InstanceContributionToHitGroupIndex = (UINT)i;
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
    if (g_rebuild) { s.inst = instBuf; s.instCount = 2; }
    return s;
}

// --rebuild: give instance i contribution i, then rebuild the top-level
// structure into the SAME destination from the SAME instance buffer.
static void RebuildTlasInPlace(Gpu& g, Scene& s) {
    D3D12_RAYTRACING_INSTANCE_DESC* d = nullptr;
    D3D12_RANGE none{ 0, 0 };
    HR(s.inst->Map(0, &none, (void**)&d), "map inst for rebuild");
    for (UINT i = 0; i < s.instCount; ++i) d[i].InstanceContributionToHitGroupIndex = i;
    s.inst->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
    ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    ti.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    ti.NumDescs = s.instCount; ti.InstanceDescs = s.inst->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    g.device->GetRaytracingAccelerationStructurePrebuildInfo(&ti, &info);
    auto scratch = CreateBuffer(g.device.Get(), info.ScratchDataSizeInBytes,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
    desc.Inputs = ti;
    desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    desc.DestAccelerationStructureData = s.tlas->GetGPUVirtualAddress();
    if (g_sameEcl) {
        // Left open; the next flush submits it together with the dispatch.
        ID3D12GraphicsCommandList4* pre = g.Pre();
        RecordBuild(g.device.Get(), pre, desc);
        UavBarrier(pre, s.tlas.Get());
        g_gpuInstCopies.push_back(scratch);   // alive until that submission
        return;
    }
    RecordBuild(g.device.Get(), g.list.Get(), desc);
    UavBarrier(g.list.Get(), s.tlas.Get());
    g.flush();
}

// --move: see g_move. The new structure replaces s.tlas, and its instance
// buffer replaces s.inst.
static void MoveTlas(Gpu& g, Scene& s) {
    D3D12_RAYTRACING_INSTANCE_DESC inst{};
    D3D12_RANGE none{ 0, 0 };
    void* p = nullptr;
    HR(s.inst->Map(0, nullptr, &p), "map inst for move");
    std::memcpy(&inst, p, sizeof(inst));
    s.inst->Unmap(0, &none);
    inst.InstanceContributionToHitGroupIndex += 1;
    auto ib = CreateBuffer(g.device.Get(), sizeof(inst), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    HR(ib->Map(0, &none, &p), "map moved inst");
    std::memcpy(p, &inst, sizeof(inst));
    ib->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
    ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    ti.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    ti.NumDescs = 1; ti.InstanceDescs = ib->GetGPUVirtualAddress();
    // Four builds at the new address, one per submission, like four frames.
    ComPtr<ID3D12Resource> moved;
    for (int i = 0; i < 4; ++i) {
        if (!moved) moved = BuildAS(g, ti);
        else {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
            g.device->GetRaytracingAccelerationStructurePrebuildInfo(&ti, &info);
            auto scratch = CreateBuffer(g.device.Get(), info.ScratchDataSizeInBytes,
                D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
            desc.Inputs = ti;
            desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
            desc.DestAccelerationStructureData = moved->GetGPUVirtualAddress();
            RecordBuild(g.device.Get(), g.list.Get(), desc);
            UavBarrier(g.list.Get(), moved.Get());
            g.flush();
        }
    }
    s.tlas = moved;
    s.inst = ib;
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
    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 0;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 1;
    // --append: u1, with its counter, has to come through a descriptor table.
    D3D12_DESCRIPTOR_RANGE logRange{};
    logRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    logRange.NumDescriptors = 1;
    logRange.BaseShaderRegister = 1;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &logRange;
    D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = g_append ? 5 : 4; rd.pParameters = params;
    if (g_bindless) rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob, err;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC vrd{};
    vrd.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    vrd.Desc_1_0 = rd;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&vrd, &blob, &err);
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

// Build the four-entry UAV descriptor table --table needs, with the REAL
// output at kTableSlot and a decoy everywhere else, so a shader that resolves
// the wrong index writes nowhere visible. `decoy` is returned so the caller
// keeps it alive.
static ComPtr<ID3D12DescriptorHeap> MakeTableHeap(
        ID3D12Device* dev, ID3D12Resource* out,
        ComPtr<ID3D12Resource>& decoy) {
    const UINT64 bytes = (UINT64)kWidth * kHeight * sizeof(Result);
    decoy = CreateBuffer(dev, bytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kTableSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");

    const UINT stride = dev->GetDescriptorHandleIncrementSize(
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
        dev->CreateUnorderedAccessView(i == kTableSlot ? out : decoy.Get(),
                                       nullptr, &ud, h);
    }
    return heap;
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
// --churn: see g_churn. Everything it creates goes into `keep`, alive until
// the list has been flushed.
static void Churn(Gpu& g, Scene& s, ID3D12PipelineState* pso, ID3D12RootSignature* rs,
                  D3D12_GPU_VIRTUAL_ADDRESS cb, D3D12_GPU_VIRTUAL_ADDRESS mask,
                  UINT64 outBytes, UINT gx, UINT gy,
                  std::vector<ComPtr<ID3D12Resource>>& keep) {
    D3D12_RAYTRACING_INSTANCE_DESC inst{};
    D3D12_RANGE none{ 0, 0 };
    void* p = nullptr;
    HR(s.inst->Map(0, nullptr, &p), "map inst for churn");
    std::memcpy(&inst, p, sizeof(inst));
    s.inst->Unmap(0, &none);

    auto decoy = CreateBuffer(g.device.Get(), outBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    keep.push_back(decoy);
    const UINT base = inst.InstanceContributionToHitGroupIndex;
    for (int k = 1; k <= g_churn; ++k) {
        // With --churnflush the second half jumps past 2048 records, which is
        // a bigger table, so spares of the old size have to give way to the new.
        const UINT jump = (g_churnFlush && k > g_churn / 2) ? 2100u : 0u;
        inst.InstanceContributionToHitGroupIndex = base + (UINT)k + jump;
        auto ib = CreateBuffer(g.device.Get(), sizeof(inst), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        HR(ib->Map(0, &none, &p), "map churn inst");
        std::memcpy(p, &inst, sizeof(inst));
        ib->Unmap(0, nullptr);
        keep.push_back(ib);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
        ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        ti.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        ti.NumDescs = 1; ti.InstanceDescs = ib->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        g.device->GetRaytracingAccelerationStructurePrebuildInfo(&ti, &info);
        auto scratch = CreateBuffer(g.device.Get(), info.ScratchDataSizeInBytes,
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        keep.push_back(scratch);

        // The previous dispatch read the structure; this build overwrites it.
        UavBarrier(g.list.Get(), s.tlas.Get());
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.Inputs = ti;
        desc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        desc.DestAccelerationStructureData = s.tlas->GetGPUVirtualAddress();
        RecordBuild(g.device.Get(), g.list.Get(), desc);
        UavBarrier(g.list.Get(), s.tlas.Get());

        BindRoots(g.list.Get(), rs, cb, s.tlas->GetGPUVirtualAddress(),
                  decoy->GetGPUVirtualAddress(), mask);
        g.list->Dispatch(gx, gy, 1);
        UavBarrier(g.list.Get(), decoy.Get());
        if (g_churnFlush) {
            g.flush();
            g.list->SetPipelineState(pso);
        }
    }
    std::printf("        %d more layouts recorded after the real dispatch, %s\n", g_churn,
                g_churnFlush ? "each submitted and waited for" : "before submitting");
}

// --append: read the counter and the records back and write them, sorted, to
// <out>.append as a count followed by the records.
static void ReadAppendLog(Gpu& g, ID3D12Resource* logBuf, ID3D12Resource* counter) {
    auto rb = CreateBuffer(g.device.Get(), kLogCount * 4ull + 4, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER b[2]{};
    for (int i = 0; i < 2; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.pResource = i ? counter : logBuf;
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    g.list->ResourceBarrier(2, b);
    g.list->CopyBufferRegion(rb.Get(), 0, logBuf, 0, kLogCount * 4ull);
    g.list->CopyBufferRegion(rb.Get(), kLogCount * 4ull, counter, 0, 4);
    g.flush();
    const uint32_t* p = nullptr;
    HR(rb->Map(0, nullptr, (void**)&p), "map append log");
    uint32_t n = p[kLogCount];
    std::vector<uint32_t> recs(p, p + (n < kLogCount ? n : kLogCount));
    rb->Unmap(0, nullptr);
    std::sort(recs.begin(), recs.end());
    const std::string path = g_outPath + ".append";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
        std::fwrite(&n, sizeof(n), 1, f);
        std::fwrite(recs.data(), sizeof(uint32_t), recs.size(), f);
        std::fclose(f);
    }
    std::printf("        %u records appended, written sorted to %s\n", n, path.c_str());
}

// --decoy / --bindless: the second live structure, the scene's one instance
// with its contribution raised by one, so record contribution + 1 means
// geometry 1 in the real scene and geometry 0 in this one.
static ComPtr<ID3D12Resource> BuildDecoyTlas(Gpu& g, const Scene& s,
                                             std::vector<ComPtr<ID3D12Resource>>& keep) {
    D3D12_RAYTRACING_INSTANCE_DESC inst{};
    D3D12_RANGE none{ 0, 0 };
    void* p = nullptr;
    HR(s.inst->Map(0, nullptr, &p), "map inst for decoy");
    std::memcpy(&inst, p, sizeof(inst));
    s.inst->Unmap(0, &none);
    inst.InstanceContributionToHitGroupIndex += 1;
    auto ib = CreateBuffer(g.device.Get(), sizeof(inst), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    HR(ib->Map(0, &none, &p), "map decoy inst");
    std::memcpy(p, &inst, sizeof(inst));
    ib->Unmap(0, nullptr);
    keep.push_back(ib);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
    ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    ti.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    ti.NumDescs = 1; ti.InstanceDescs = ib->GetGPUVirtualAddress();
    return BuildAS(g, ti);
}

// --bindless: slot 3 of a shader-visible heap holds the real scene, copied
// there from a staging heap over a decoy written first; slots 1, 2 and 4
// hold the decoy. The same arrangement tier11/gitest.cpp uses.
static ComPtr<ID3D12DescriptorHeap> MakeSceneHeap(ID3D12Device* dev, ID3D12Resource* real,
        ID3D12Resource* decoy, ComPtr<ID3D12DescriptorHeap>& staging) {
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 6,
                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "scene heap");
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; hd.NumDescriptors = 2;
    HR(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&staging)), "scene staging heap");
    auto srv = [&](ID3D12Resource* as, D3D12_CPU_DESCRIPTOR_HANDLE at) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.RaytracingAccelerationStructure.Location = as->GetGPUVirtualAddress();
        dev->CreateShaderResourceView(nullptr, &sd, at);
    };
    auto at = [&](ID3D12DescriptorHeap* h, UINT i) {
        D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart();
        c.ptr += (SIZE_T)i * inc;
        return c;
    };
    srv(decoy, at(staging.Get(), 0));
    srv(real, at(staging.Get(), 1));
    srv(decoy, at(heap.Get(), 3));
    dev->CopyDescriptorsSimple(1, at(heap.Get(), 1), at(staging.Get(), 0),
                               D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dev->CopyDescriptorsSimple(1, at(heap.Get(), 2), at(staging.Get(), 0),
                               D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE dst[2] = { at(heap.Get(), 3), at(heap.Get(), 4) };
    D3D12_CPU_DESCRIPTOR_HANDLE src[2] = { at(staging.Get(), 1), at(staging.Get(), 0) };
    UINT ones[2] = { 1, 1 };
    dev->CopyDescriptors(2, dst, ones, 2, src, ones, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return heap;
}

static void RunRayQuery(Gpu& g, Dxc& dxc, const Scene& s,
        ID3D12Resource* cb, ID3D12Resource* out, const char* hlsl) {
    // --table has to reach this side too, or a shader written against a
    // descriptor table cannot run as a RayQuery compute shader at all, which
    // is what kept the array cases out of the proxy's dispatch path.
    auto rs = g_table ? MakeRootSigTable(g.device.Get())
                      : MakeRootSig(g.device.Get());
    std::string fromFile;
    if (g_csFile) {
        auto bytes = ReadAll(g_csFile);
        fromFile.assign((const char*)bytes.data(), bytes.size());
        hlsl = fromFile.c_str();
        std::printf("        compute shader from %s\n", g_csFile);
    }
    // A shader file named *sm66* is compiled at 6.6, which is what Unreal
    // uses. Until 0.40.0 everything here was 6.5, so the dispatch suite's
    // `sm66` case ran a 6.5 shader and the 6.6 form never reached the proxy.
    const bool sm66 = g_csFile && std::strstr(g_csFile, "sm66");
    ComPtr<IDxcBlob> cs = dxc.compile(hlsl, L"main", sm66 ? L"cs_6_6" : L"cs_6_5");
    if (sm66) std::printf("        compiled at cs_6_6\n");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    ComPtr<ID3D12PipelineState> pso;
    if (g_stream) {
        // Laid out the way an engine lays it out, which is the point: the root
        // signature comes FIRST, so a walker that stops at the first
        // non-shader subobject never reaches the CS at all.
        struct alignas(void*) SubRootSig {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type =
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
            ID3D12RootSignature* value;
        };
        struct alignas(void*) SubCS {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type =
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
            D3D12_SHADER_BYTECODE value;
        };
        struct alignas(void*) SubNodeMask {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type =
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK;
            UINT value;
        };
        struct alignas(void*) SubFlags {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type =
                D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS;
            D3D12_PIPELINE_STATE_FLAGS value;
        };
        struct alignas(void*) Stream {
            SubRootSig rootSig;
            SubNodeMask nodeMask;
            SubCS cs;
            SubFlags flags;
        } stream{};
        stream.rootSig.value = rs.Get();
        stream.nodeMask.value = 0;
        stream.cs.value = pd.CS;
        stream.flags.value = D3D12_PIPELINE_STATE_FLAG_NONE;

        ComPtr<ID3D12Device2> dev2;
        HR(g.device.As(&dev2), "ID3D12Device2 for CreatePipelineState");
        D3D12_PIPELINE_STATE_STREAM_DESC sd{};
        sd.SizeInBytes = sizeof(stream);
        sd.pPipelineStateSubobjectStream = &stream;
        HR(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)), "CreatePipelineState (stream)");
        std::printf("        compute PSO created through CreatePipelineState (stream form)\n");
    } else {
        HR(g.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)),
           "CreateComputePipelineState");
    }

    auto mask = MakeAlphaMask(g.device.Get());
    std::vector<ComPtr<ID3D12Resource>> decoyKeep;
    ComPtr<ID3D12Resource> decoyTlas;
    ComPtr<ID3D12DescriptorHeap> sceneHeap, sceneStaging;
    if ((g_decoy || g_bindless) && s.inst) {
        decoyTlas = BuildDecoyTlas(g, s, decoyKeep);
        std::printf("        a second live top-level structure, its records conflicting\n");
    }
    if (g_bindless && decoyTlas)
        sceneHeap = MakeSceneHeap(g.device.Get(), s.tlas.Get(), decoyTlas.Get(), sceneStaging);
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> decoy;
    if (g_table) heap = MakeTableHeap(g.device.Get(), out, decoy);

    // --append: the log, its counter, and a one-entry heap holding the UAV.
    ComPtr<ID3D12Resource> logBuf, logCounter;
    ComPtr<ID3D12DescriptorHeap> logHeap;
    if (g_append && !g_table) {
        logBuf = CreateBuffer(g.device.Get(), kLogCount * 4ull, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        logCounter = CreateBuffer(g.device.Get(), 4, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&logHeap)), "log heap");
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kLogCount;
        ud.Buffer.StructureByteStride = 4;
        ud.Buffer.CounterOffsetInBytes = 0;
        g.device->CreateUnorderedAccessView(logBuf.Get(), logCounter.Get(), &ud,
            logHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // Reset the list WITH the pipeline state, not with null.
    //
    // This is how Unreal reuses a command list and it is not what this harness
    // used to do, so the shim's stand-in PSO reached the real driver through
    // Reset, through ClearState and through CreateCommandList while every test
    // here passed. A GPU crash in a real game, and nothing here could see it.
    // SetPipelineState is still called after, exactly as an engine would.
    auto start = [&]() {
    HR(g.list->Close(), "cmdlist Close before PSO reset");
    HR(g.list->Reset(g.alloc.Get(), pso.Get()), "cmdlist Reset with the PSO");

    g.list->SetPipelineState(pso.Get());
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
        if (logHeap) {
            ID3D12DescriptorHeap* heaps[] = { logHeap.Get() };
            g.list->SetDescriptorHeaps(1, heaps);
        }
        if (sceneHeap) {
            ID3D12DescriptorHeap* heaps[] = { sceneHeap.Get() };
            g.list->SetDescriptorHeaps(1, heaps);
        }
        // --bindless: the root SRV at t0 holds the DECOY; the shader never
        // reads t0, so only a shim that guesses from root SRVs would use it.
        BindRoots(g.list.Get(), rs.Get(), cb->GetGPUVirtualAddress(),
                  (sceneHeap ? decoyTlas : s.tlas)->GetGPUVirtualAddress(),
                  out->GetGPUVirtualAddress(), mask->GetGPUVirtualAddress());
        if (logHeap)
            g.list->SetComputeRootDescriptorTable(
                4, logHeap->GetGPUDescriptorHandleForHeapStart());
    }
    };
    start();
    if (g_move && s.inst) {
        g.list->Dispatch((kWidth + 7) / 8, (kHeight + 7) / 8, 1);
        UavBarrier(g.list.Get(), out);
        g.flush();
        MoveTlas(g, const_cast<Scene&>(s));
        std::printf("        top-level structure MOVED to a new address and built there 4 times\n");
        start();
    }
    if (g_rebuild && s.inst) {
        // Dispatch against the all-zeros scene first, so the shim learns it
        // and builds its table for it. Then rebuild in place and go again.
        g.list->Dispatch((kWidth + 7) / 8, (kHeight + 7) / 8, 1);
        UavBarrier(g.list.Get(), out);
        g.flush();
        RebuildTlasInPlace(g, const_cast<Scene&>(s));
        std::printf("        top-level structure rebuilt IN PLACE with new contributions\n");
        start();
    }
    // Group count sized for the smallest thread group any of these shaders
    // uses. A 16x16 shader then gets more groups than it needs, which is safe
    // because every one of them bounds checks; too FEW groups would not be.
    const UINT gx = (kWidth + 7) / 8, gy = (kHeight + 7) / 8;
    if (!g_indirect && !g_indirectUp) {
        g.list->Dispatch(gx, gy, 1);
        UavBarrier(g.list.Get(), out);
        std::vector<ComPtr<ID3D12Resource>> keep;
        if (g_blasReuse && s.blas && s.vb) {
            // The same address, one geometry: the first quadrant only.
            D3D12_RAYTRACING_GEOMETRY_DESC geo{};
            geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
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
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
            g.device->GetRaytracingAccelerationStructurePrebuildInfo(&bi, &info);
            auto scratch = CreateBuffer(g.device.Get(), info.ScratchDataSizeInBytes,
                D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            keep.push_back(scratch);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
            d.Inputs = bi;
            d.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
            d.DestAccelerationStructureData = s.blas->GetGPUVirtualAddress();
            g.Post()->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
        }
        if (g_churn && s.inst && !g_table)
            Churn(g, const_cast<Scene&>(s), pso.Get(), rs.Get(), cb->GetGPUVirtualAddress(),
                  mask->GetGPUVirtualAddress(), out->GetDesc().Width, gx, gy, keep);
        g.flush();
        if (logHeap) ReadAppendLog(g, logBuf.Get(), logCounter.Get());
        return;
    }

    D3D12_INDIRECT_ARGUMENT_DESC ad{};
    ad.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sd{};
    sd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &ad;
    ComPtr<ID3D12CommandSignature> sig;
    HR(g.device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&sig)),
       "CreateCommandSignature(DISPATCH)");

    // 64 words of decoys, 3 groups each way, with the real arguments at byte 36.
    const UINT64 kArgOffset = 36;
    UINT words[64];
    for (UINT& w : words) w = 3;
    words[kArgOffset / 4 + 0] = gx;
    words[kArgOffset / 4 + 1] = gy;
    words[kArgOffset / 4 + 2] = 1;
    auto upload = CreateBuffer(g.device.Get(), sizeof(words), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(upload->Map(0, &none, &p), "map indirect args");
    std::memcpy(p, words, sizeof(words));
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12Resource> args = upload;
    if (g_indirect) {
        args = CreateBuffer(g.device.Get(), sizeof(words), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyBufferRegion(args.Get(), 0, upload.Get(), 0, sizeof(words));
        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = args.Get();
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        g.list->ResourceBarrier(1, &tb);
    }
    std::printf("        dispatched INDIRECTLY, %ux%ux1 groups at byte %llu of a %s buffer\n",
                gx, gy, (unsigned long long)kArgOffset,
                g_indirect ? "GPU-written" : "upload");
    g.list->ExecuteIndirect(sig.Get(), 1, args.Get(), kArgOffset, nullptr, 0);
    UavBarrier(g.list.Get(), out);
    g.flush();
}

// A shader table with one record: just a 32-byte identifier, table 64-aligned.
// Records inside one table are aligned to 32, the table itself to 64.
static const UINT kHitRecStride = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;

// Two hit group records in one table, for a library that commits both kinds.
static ComPtr<ID3D12Resource> MakeSBT2(ID3D12Device* dev, const void* a,
                                       const void* b) {
    const UINT idSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
    UINT64 size = (kHitRecStride * 2 + D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT - 1)
                & ~(UINT64)(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT - 1);
    auto buf = CreateBuffer(dev, size, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* p = nullptr; D3D12_RANGE none{ 0, 0 };
    HR(buf->Map(0, &none, (void**)&p), "map SBT2");
    std::memset(p, 0, (size_t)size);
    std::memcpy(p, a, idSize);
    std::memcpy(p + kHitRecStride, b, idSize);
    buf->Unmap(0, nullptr);
    return buf;
}

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
    if (g_table) heap = MakeTableHeap(g.device.Get(), out, decoy);

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
    if (g_both) {
        // A library that commits both kinds exports an any-hit AND an
        // intersection shader, and two closest-hits, because the committed
        // status differs. So it needs two hit groups: this one for triangles
        // and hgProc below for procedural primitives.
        hg.AnyHitShaderImport = L"AnyHit";
    } else if (g_proc) {
        // A procedural hit group takes an intersection shader instead of an
        // any-hit one, and is a different hit group TYPE.
        hg.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        hg.IntersectionShaderImport = L"Isect";
    } else if (anyHit) {
        hg.AnyHitShaderImport = L"AnyHit";
    }
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg });

    D3D12_HIT_GROUP_DESC hgProc{};
    if (g_both) {
        hgProc.HitGroupExport = L"HitGroupProc";
        hgProc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        hgProc.IntersectionShaderImport = L"Isect";
        hgProc.ClosestHitShaderImport = L"ClosestHitProc";
        subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgProc });
    }

    D3D12_RAYTRACING_SHADER_CONFIG sc{};
    // Must be at least the payload the rewriter emits, PAYLOAD_BYTES in
    // phase5/rewriter/lower.py, currently 84: t, bary, hit, the instance,
    // primitive and instance ID, the hit kind, and the 3x4 world-to-object
    // matrix. It is a MAXIMUM, so declaring
    // 28 costs the 16-byte hand-written shaders nothing. Getting this wrong
    // shows up as CreateStateObject returning E_INVALIDARG, which is a real
    // coupling: the payload size is part of the state object contract, not a
    // free choice for whatever generates the shaders.
    sc.MaxPayloadSizeInBytes = 92;   // must match kPayloadBytes in rq_lower.cpp
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
    // One record per InstanceContributionToHitGroupIndex the scene uses. With
    // --both that is two: the triangle instance contributes 0 and the
    // procedural one 1, which is what --mixed --contrib builds.
    auto sbtHit = g_both
        ? MakeSBT2(g.device.Get(), props->GetShaderIdentifier(L"HitGroup"),
                   props->GetShaderIdentifier(L"HitGroupProc"))
        : MakeSBT(g.device.Get(), props->GetShaderIdentifier(L"HitGroup"));

    D3D12_DISPATCH_RAYS_DESC dr{};
    const UINT idSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dr.RayGenerationShaderRecord.StartAddress = sbtRayGen->GetGPUVirtualAddress();
    dr.RayGenerationShaderRecord.SizeInBytes = idSize;
    dr.MissShaderTable.StartAddress = sbtMiss->GetGPUVirtualAddress();
    dr.MissShaderTable.SizeInBytes = idSize;
    dr.MissShaderTable.StrideInBytes = idSize;
    dr.HitGroupTable.StartAddress = sbtHit->GetGPUVirtualAddress();
    dr.HitGroupTable.SizeInBytes = g_both ? kHitRecStride * 2 : idSize;
    dr.HitGroupTable.StrideInBytes = g_both ? kHitRecStride : idSize;
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
    g_outPath = outPath;

    // Poison only the LOWERED side. Corrupting both would leave them agreeing
    // with each other, so the diff would still report MATCH and would prove
    // nothing about whether it can see an IR change at all.
    g_poisonNow = (method == Method::TraceRay);

    ComPtr<IDXGIFactory6> factory;
    UINT flags = 0;
    // The debug layer is the only thing that can say whether a shader table we
    // built is VALID as opposed to merely producing the number we wanted, so it
    // has to be reachable from a release build too.
    bool wantDebug = false;
#ifdef _DEBUG
    wantDebug = true;
#endif
    { char buf[8]; size_t n = 0;
      if (getenv_s(&n, buf, sizeof(buf), "DXR_TIER11_DEBUGLAYER") == 0 && n > 1 && buf[0] == '1')
          wantDebug = true; }
    if (wantDebug) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
        flags = DXGI_CREATE_FACTORY_DEBUG;
    }
    HR(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    auto adapt = PickAdapter(factory.Get(), adapter);

    ComPtr<ID3D12Device5> device;
    HR(D3D12CreateDevice(adapt.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
       "D3D12CreateDevice");

    // The debug layer reports through OutputDebugString, which a console never
    // sees, so its silence in a terminal means nothing at all. Take the
    // messages from the InfoQueue and print them, the way the Phase 4 probe
    // does, or this switch is decoration.
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (wantDebug) {
        if (SUCCEEDED(device.As(&infoQueue)))
            std::printf("        debug layer: on\n");
        else
            std::printf("        debug layer: requested, ID3D12InfoQueue unavailable\n");
    }
    auto drainDebug = [&infoQueue](const char* where) {
        if (!infoQueue) return;
        const UINT64 n = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i) {
            SIZE_T len = 0;
            if (FAILED(infoQueue->GetMessage(i, nullptr, &len)) || !len) continue;
            std::vector<char> raw(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(raw.data());
            if (FAILED(infoQueue->GetMessage(i, m, &len))) continue;
            if (m->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
            std::printf("        [debug-layer/%s] %.*s\n", where,
                        (int)m->DescriptionByteLength, m->pDescription);
        }
        infoQueue->ClearStoredMessages();
    };

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
    Scene scene = g_mixed ? BuildSceneMixed(g, pattern == Pattern::Opaque)
                : g_proc  ? BuildSceneProc(g, pattern == Pattern::Opaque)
                : g_geom  ? BuildSceneGeom(g, pattern == Pattern::Opaque)
                : g_multi ? BuildSceneMulti(g, pattern == Pattern::Opaque)
                          : BuildScene(g, pattern == Pattern::Opaque);

    // Constant buffer.
    SceneCB cbData{ kWidth, kHeight, kHalfExtent, kCamZ, kTMin, kTMax, 0, 0 };
    if (g_bindless) {   // the scene's heap slot, as a uint in _pad.x
        const uint32_t slot = 3;
        std::memcpy(&cbData.pad0, &slot, 4);
    }
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
    if (g_prefill) {
        auto up = CreateBuffer(device.Get(), outSize, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        Result* pr = nullptr; D3D12_RANGE nr{ 0, 0 };
        HR(up->Map(0, &nr, (void**)&pr), "map prefill");
        for (UINT i = 0; i < kWidth * kHeight; ++i) {
            Result r{};
            r.hit = (((i * 2654435761u) >> 30) + g_prefill) & 3u;
            pr[i] = r;
        }
        up->Unmap(0, nullptr);
        Transition(g.list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyBufferRegion(out.Get(), 0, up.Get(), 0, outSize);
        Transition(g.list.Get(), out.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.flush();
    }

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
    drainDebug("trial");
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
            if (std::strcmp(argv[i], "--proc") == 0) {
                g_proc = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--both") == 0) {
                g_both = true;
                continue;
            }
            if (std::strcmp(argv[i], "--mixed") == 0) {
                g_mixed = true;
                continue;
            }
            if (std::strcmp(argv[i], "--contrib") == 0) {
                g_contrib = true;
                continue;
            }
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
            if (std::strcmp(argv[i], "--geom") == 0) {
                g_geom = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--nullinst") == 0) {
                g_nullInst = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--blasclone") == 0 || std::strcmp(argv[i], "--tlasclone") == 0 ||
                std::strcmp(argv[i], "--deserialize") == 0) {
                (argv[i][2] == 'b' ? g_blasClone : argv[i][2] == 't' ? g_tlasClone : g_deserialize) = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--blasreuse") == 0) {
                g_blasReuse = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--empty") == 0 || std::strcmp(argv[i], "--sameecl") == 0) {
                (argv[i][2] == 'e' ? g_empty : g_sameEcl) = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--gpuinst") == 0) {
                g_gpuinst = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--stream") == 0) {
                g_stream = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--prefill") == 0 && i + 1 < argc) {
                g_prefill = (unsigned)std::strtoul(argv[i + 1], nullptr, 10);
                for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
                argc -= 2; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--append") == 0) {
                g_append = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--decoy") == 0 || std::strcmp(argv[i], "--bindless") == 0) {
                (argv[i][2] == 'd' ? g_decoy : g_bindless) = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--move") == 0) {
                g_move = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--churnflush") == 0) {
                g_churnFlush = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--churn") == 0 && i + 1 < argc) {
                g_churn = std::atoi(argv[i + 1]);
                for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
                argc -= 2; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--rebuild") == 0) {
                g_rebuild = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
                continue;
            }
            if (std::strcmp(argv[i], "--indirect") == 0 ||
                std::strcmp(argv[i], "--indirectup") == 0) {
                (argv[i][10] ? g_indirectUp : g_indirect) = true;
                for (int j = i; j + 1 < argc; ++j) argv[j] = argv[j + 1];
                --argc; --i;
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
