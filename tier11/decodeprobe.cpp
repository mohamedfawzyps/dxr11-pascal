// Can the shim learn what a DESERIALIZED acceleration structure contains?
// (Tier 1.1 item a's refusal "an instance on a deserialized structure".)
//
// A deserialized structure's contents are in a driver-opaque blob, so the
// shim cannot know a bottom-level one's geometry count or types, or a
// top-level one's instances. D3D12 has one documented way to ask the driver:
// CopyRaytracingAccelerationStructure with VISUALIZATION_DECODE_FOR_TOOLS,
// which writes a header (type, NumDescs) followed by the geometry or instance
// descriptions (d3d12.h, D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_
// VISUALIZATION_HEADER). This measures whether it is available and whether
// what it writes is right: a bottom-level structure of 2 triangle geometries
// (flags OPAQUE, NONE), one of 1 procedural (NO_DUPLICATE_ANYHIT), and a
// top-level one of 2 instances, one on each (contributions 3 and 7, ids 11
// and 12), each built, serialized, deserialized, then decoded; the original
// decoded too. (A first version put both kinds in ONE bottom-level
// structure, which DXR does not allow: WARP built it, the GTX 1070 removed
// its device.)
//
//   decodeprobe.exe [warp|hw] [-debug]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;

#ifndef DXRTEST_AGILITY_VERSION
  #define DXRTEST_AGILITY_VERSION 619u
#endif
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = DXRTEST_AGILITY_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

static ComPtr<ID3D12Device5> g_dev;
static ComPtr<ID3D12CommandQueue> g_q;
static ComPtr<ID3D12CommandAllocator> g_al;
static ComPtr<ID3D12GraphicsCommandList4> g_cl;
static ComPtr<ID3D12Fence> g_f;
static UINT64 g_fv = 0;
static std::vector<ComPtr<ID3D12Resource>> g_keep;

static ID3D12Resource* Buf(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES st, bool uav) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size ? size : 256; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    const HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) {
        std::printf("CreateCommittedResource failed: %llu bytes, heap %d, state 0x%X, hr 0x%08lX\n",
                    (unsigned long long)rd.Width, (int)heap, (unsigned)st, (unsigned long)hr);
        std::exit(1);
    }
    g_keep.push_back(r);
    return r.Get();
}
static ID3D12Resource* Uav(UINT64 size) {
    return Buf(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
}
static ID3D12Resource* AsBuf(UINT64 size) {
    return Buf(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
}
static void UavBarrier() {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    g_cl->ResourceBarrier(1, &b);
}
static bool Flush() {
    if (FAILED(g_cl->Close())) { std::printf("Close failed\n"); return false; }
    ID3D12CommandList* l[] = { g_cl.Get() };
    g_q->ExecuteCommandLists(1, l);
    g_q->Signal(g_f.Get(), ++g_fv);
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_f->SetEventOnCompletion(g_fv, e);
    WaitForSingleObject(e, 10000);
    CloseHandle(e);
    g_al->Reset();
    g_cl->Reset(g_al.Get(), nullptr);
    const HRESULT r = g_dev->GetDeviceRemovedReason();
    if (FAILED(r)) { std::printf("DEVICE REMOVED, reason 0x%08lX\n", (unsigned long)r); return false; }
    return true;
}
// Copies `bytes` of a UAV buffer to the CPU.
static std::vector<uint8_t> Read(ID3D12Resource* src, UINT64 bytes) {
    ID3D12Resource* rb = Buf(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_cl->ResourceBarrier(1, &b);
    g_cl->CopyBufferRegion(rb, 0, src, 0, bytes);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_cl->ResourceBarrier(1, &b);
    std::vector<uint8_t> out((size_t)bytes);
    if (!Flush()) return {};
    void* m = nullptr;
    rb->Map(0, nullptr, &m);
    std::memcpy(out.data(), m, (size_t)bytes);
    rb->Unmap(0, nullptr);
    return out;
}
static UINT64 PostbuildSize(ID3D12Resource* as, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE t) {
    ID3D12Resource* d = Uav(16);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pd{ d->GetGPUVirtualAddress(), t };
    D3D12_GPU_VIRTUAL_ADDRESS a = as->GetGPUVirtualAddress();
    g_cl->EmitRaytracingAccelerationStructurePostbuildInfo(&pd, 1, &a);
    UavBarrier();
    auto v = Read(d, 16);
    UINT64 s = 0;
    if (v.size() >= 8) std::memcpy(&s, v.data(), 8);
    return s;
}
static ID3D12Resource* Build(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in) {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
    ID3D12Resource* as = AsBuf(pi.ResultDataMaxSizeInBytes);
    ID3D12Resource* scratch = Uav(pi.ScratchDataSizeInBytes);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
    bd.Inputs = in;
    bd.DestAccelerationStructureData = as->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    g_cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    UavBarrier();
    return Flush() ? as : nullptr;
}
static ID3D12Resource* SerializeDeserialize(ID3D12Resource* as) {
    const UINT64 ser = PostbuildSize(as, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION);
    std::printf("  serialized size %llu\n", (unsigned long long)ser);
    if (!ser) return nullptr;
    ID3D12Resource* blob = Uav(ser);
    g_cl->CopyRaytracingAccelerationStructure(blob->GetGPUVirtualAddress(), as->GetGPUVirtualAddress(),
                                              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
    UavBarrier();
    auto h = Read(blob, sizeof(D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER));
    if (h.empty()) return nullptr;
    D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER hdr{};
    std::memcpy(&hdr, h.data(), sizeof(hdr));
    std::printf("  deserialized size %llu, %llu bottom-level pointers after the header\n",
                (unsigned long long)hdr.DeserializedSizeInBytes,
                (unsigned long long)hdr.NumBottomLevelAccelerationStructurePointersAfterHeader);
    ID3D12Resource* out = AsBuf(hdr.DeserializedSizeInBytes);
    g_cl->CopyRaytracingAccelerationStructure(out->GetGPUVirtualAddress(), blob->GetGPUVirtualAddress(),
                                              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
    UavBarrier();
    return Flush() ? out : nullptr;
}
// Decodes `as` for tools and prints what it says; false when it cannot be.
static bool Decode(ID3D12Resource* as, const char* what) {
    const UINT64 size = PostbuildSize(as, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION);
    std::printf("%s: decoded size %llu\n", what, (unsigned long long)size);
    if (!size) return false;
    ID3D12Resource* d = Uav(size);
    g_cl->CopyRaytracingAccelerationStructure(d->GetGPUVirtualAddress(), as->GetGPUVirtualAddress(),
                                              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS);
    UavBarrier();
    auto v = Read(d, size);
    if (v.size() < sizeof(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER)) return false;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER hdr{};
    std::memcpy(&hdr, v.data(), sizeof(hdr));
    std::printf("  header: type %u, %u descriptions\n", (unsigned)hdr.Type, hdr.NumDescs);
    size_t at = sizeof(hdr);
    for (UINT i = 0; i < hdr.NumDescs && i < 16; ++i) {
        if (hdr.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
            // "4 bytes of padding between GeometryDesc structs" (d3d12.h).
            at = (at + 7) & ~(size_t)7;
            if (at + sizeof(D3D12_RAYTRACING_GEOMETRY_DESC) > v.size()) break;
            D3D12_RAYTRACING_GEOMETRY_DESC g{};
            std::memcpy(&g, v.data() + at, sizeof(g));
            if (g.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES)
                std::printf("  geometry %u: type %u flags %u, %u vertices, %u indices\n", i, (unsigned)g.Type,
                            (unsigned)g.Flags, g.Triangles.VertexCount, g.Triangles.IndexCount);
            else
                std::printf("  geometry %u: type %u flags %u, %llu AABBs\n", i, (unsigned)g.Type,
                            (unsigned)g.Flags, (unsigned long long)g.AABBs.AABBCount);
            at += sizeof(g);
        } else {
            // Right after the 8-byte header: WARP writes 8 + 64 * NumDescs.
            if (at + sizeof(D3D12_RAYTRACING_INSTANCE_DESC) > v.size()) break;
            D3D12_RAYTRACING_INSTANCE_DESC x{};
            std::memcpy(&x, v.data() + at, sizeof(x));
            std::printf("  instance %u: id %u, contribution %u, mask 0x%X, structure 0x%llX\n", i,
                        x.InstanceID, x.InstanceContributionToHitGroupIndex, x.InstanceMask,
                        (unsigned long long)x.AccelerationStructure);
            at += sizeof(x);
        }
    }
    return true;
}

int main(int argc, char** argv) {
    const bool warp = argc > 1 && !std::strcmp(argv[1], "warp");
    const bool debug = argc > 2 && !std::strcmp(argv[2], "-debug");
    if (debug) {
        ComPtr<ID3D12Debug> d;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d)))) d->EnableDebugLayer();
    }
    DWORD devmode = 0, sz = sizeof(devmode);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
                 L"AllowDevelopmentWithoutDevLicense", RRF_RT_REG_DWORD, nullptr, &devmode, &sz);
    std::printf("developer mode: %s\n", devmode ? "ON" : "off");
    ComPtr<IDXGIFactory6> f; CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    ComPtr<IDXGIAdapter1> a;
    if (warp) f->EnumWarpAdapter(IID_PPV_ARGS(&a));
    else f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a));
    DXGI_ADAPTER_DESC1 ad{}; a->GetDesc1(&ad);
    std::printf("adapter: %ls\n", ad.Description);
    if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev)))) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_q));
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_al));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_al.Get(), nullptr, IID_PPV_ARGS(&g_cl));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_f));

    // Geometry: two triangles and one box.
    const float verts[18] = { 0, 0, 0, 1, 0, 0, 0, 1, 0,   2, 0, 0, 3, 0, 0, 2, 1, 0 };
    const D3D12_RAYTRACING_AABB box{ 4, 0, -0.5f, 5, 1, 0.5f };
    ID3D12Resource* vb = Buf(sizeof(verts), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    ID3D12Resource* bb = Buf(sizeof(box), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    void* m = nullptr;
    vb->Map(0, nullptr, &m); std::memcpy(m, verts, sizeof(verts)); vb->Unmap(0, nullptr);
    bb->Map(0, nullptr, &m); std::memcpy(m, &box, sizeof(box)); bb->Unmap(0, nullptr);
    D3D12_RAYTRACING_GEOMETRY_DESC g[3]{};
    for (int i = 0; i < 2; ++i) {
        g[i].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g[i].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g[i].Triangles.VertexCount = 3;
        g[i].Triangles.VertexBuffer = { vb->GetGPUVirtualAddress() + i * 36, 12 };
    }
    g[0].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    g[2].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    g[2].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
    g[2].AABBs.AABBCount = 1;
    g[2].AABBs.AABBs = { bb->GetGPUVirtualAddress(), sizeof(box) };
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bl{};
    bl.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bl.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    bl.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bl.NumDescs = 2;
    bl.pGeometryDescs = g;
    ID3D12Resource* blas = Build(bl);
    if (!blas) return 1;
    bl.NumDescs = 1;
    bl.pGeometryDescs = g + 2;
    ID3D12Resource* blasProc = Build(bl);
    if (!blasProc) return 1;

    D3D12_RAYTRACING_INSTANCE_DESC inst[2]{};
    for (int i = 0; i < 2; ++i) {
        inst[i].Transform[0][0] = inst[i].Transform[1][1] = inst[i].Transform[2][2] = 1.0f;
        inst[i].Transform[1][3] = (float)(2 * i);
        inst[i].InstanceID = 11 + i;
        inst[i].InstanceMask = 0xFF;
        inst[i].InstanceContributionToHitGroupIndex = i ? 7 : 3;
        inst[i].AccelerationStructure = (i ? blasProc : blas)->GetGPUVirtualAddress();
    }
    ID3D12Resource* ib = Buf(sizeof(inst), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    ib->Map(0, nullptr, &m); std::memcpy(m, inst, sizeof(inst)); ib->Unmap(0, nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tl{};
    tl.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tl.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tl.NumDescs = 2;
    tl.InstanceDescs = ib->GetGPUVirtualAddress();
    ID3D12Resource* tlas = Build(tl);
    if (!tlas) return 1;
    std::printf("bottom-level 0x%llX (triangles) and 0x%llX (procedural), top-level 0x%llX built\n",
                (unsigned long long)blas->GetGPUVirtualAddress(),
                (unsigned long long)blasProc->GetGPUVirtualAddress(),
                (unsigned long long)tlas->GetGPUVirtualAddress());

    bool ok = true;
    for (ID3D12Resource* b : { blas, blasProc }) {
        ok = Decode(b, "bottom-level, as built") && ok;
        std::printf("bottom-level, serialized and deserialized:\n");
        ID3D12Resource* b2 = SerializeDeserialize(b);
        ok = b2 && Decode(b2, "bottom-level, deserialized") && ok;
    }
    ok = Decode(tlas, "top-level, as built") && ok;
    std::printf("top-level, serialized and deserialized:\n");
    ID3D12Resource* tlas2 = SerializeDeserialize(tlas);
    ok = tlas2 && Decode(tlas2, "top-level, deserialized") && ok;
    const HRESULT r = g_dev->GetDeviceRemovedReason();
    std::printf("\n%s; device removed reason after: 0x%08lX\n", ok ? "DECODED" : "NOT DECODED", (unsigned long)r);
    return ok && SUCCEEDED(r) ? 0 : 1;
}
