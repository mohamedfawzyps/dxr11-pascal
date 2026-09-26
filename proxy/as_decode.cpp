// What a deserialized acceleration structure contains. See the header.
#include "as_decode.h"

#include "as_tracker.h"
#include "d3d12_command_list.h"
#include "proxy_log.h"

#include <wrl/client.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace asdecode {
namespace {

// The size queries' buffers, pooled: a committed resource takes at least
// 64 KB, and an engine may deserialize thousands of structures. 8 bytes a
// query; `info` stays in UNORDERED_ACCESS between queries, so lists recorded
// on several threads agree on its state.
const UINT kSlots = 8192;
struct Chunk {
    ComPtr<ID3D12Resource> info, readback;
    UINT used = 0;
};
std::shared_ptr<Chunk> g_chunk;
ID3D12Device* g_chunkDevice = nullptr;

struct Query {
    D3D12_GPU_VIRTUAL_ADDRESS dst = 0;
    astrack::DeserializedAt at;
    std::shared_ptr<Chunk> chunk;            // the size, on the GPU and for the CPU
    UINT offset = 0;
    const void* owner = nullptr;
    UINT segment = 0;
    ID3D12CommandQueue* queue = nullptr;     // set when stamped
    UINT64 fenceValue = 0;
};
struct QueueFence {
    ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
};

std::mutex g_lock;
std::vector<Query> g_queries;
std::map<ID3D12CommandQueue*, QueueFence> g_fences;

ComPtr<ID3D12Resource> Buffer(ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE heap,
                              D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                            IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

void Transition(ID3D12GraphicsCommandList4* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
                D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &x);
}

void UavBarrier(ID3D12GraphicsCommandList4* cl) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl->ResourceBarrier(1, &x);
}

bool Submitted(const void* owner, ID3D12CommandList* const* lists, UINT count) {
    for (UINT i = 0; i < count; ++i)
        if (static_cast<const void*>(Dxr11CommandList::From(lists[i])) == owner) return true;
    return false;
}

}  // namespace

void Note(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, D3D12_GPU_VIRTUAL_ADDRESS dst,
          const void* owner, UINT segment) {
    if (!cl || !dev || !dst) return;
    Query q;
    q.dst = dst;
    q.owner = owner;
    q.segment = segment;
    if (!astrack::DeserializedNow(dst, &q.at)) return;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (!g_chunk || g_chunk->used == kSlots || g_chunkDevice != dev) {
            auto c = std::make_shared<Chunk>();
            c->info = Buffer(dev, kSlots * 8ull, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
            c->readback = Buffer(dev, kSlots * 8ull, D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_STATE_COPY_DEST, false);
            if (!c->info || !c->readback) {
                ProxyLog("[dxr-tier-11-proxy-log] deserialized structure at 0x%llX: could not create "
                         "the buffers to ask its size; it stays unknown\n", (unsigned long long)dst);
                return;
            }
            g_chunk = c;
            g_chunkDevice = dev;
        }
        q.chunk = g_chunk;
        q.offset = 8 * g_chunk->used++;
    }
    // The deserialize has to have written it before its size is asked.
    UavBarrier(cl);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pd{};
    pd.DestBuffer = q.chunk->info->GetGPUVirtualAddress() + q.offset;
    pd.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TOOLS_VISUALIZATION;
    cl->EmitRaytracingAccelerationStructurePostbuildInfo(&pd, 1, &dst);
    ID3D12Resource* info = q.chunk->info.Get();
    Transition(cl, info, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(q.chunk->readback.Get(), q.offset, info, q.offset, 8);
    Transition(cl, info, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    std::lock_guard<std::mutex> g(g_lock);
    g_queries.push_back(q);
}

bool Pending() {
    std::lock_guard<std::mutex> g(g_lock);
    return !g_queries.empty();
}

void AfterSubmit(ID3D12CommandQueue* queue, ID3D12CommandList* const* lists, UINT count) {
    if (!queue) return;
    std::lock_guard<std::mutex> g(g_lock);
    bool any = false;
    for (const Query& q : g_queries)
        if (!q.fenceValue && Submitted(q.owner, lists, count)) { any = true; break; }
    if (!any) return;
    QueueFence& qf = g_fences[queue];
    if (!qf.fence) {
        ComPtr<ID3D12Device> dev;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev))))
            dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&qf.fence));
    }
    if (!qf.fence || FAILED(queue->Signal(qf.fence.Get(), qf.value + 1))) return;
    ++qf.value;
    for (Query& q : g_queries)
        if (!q.fenceValue && Submitted(q.owner, lists, count)) {
            q.queue = queue;
            q.fenceValue = qf.value;
        }
}

void DropUnsubmitted(const void* owner) {
    if (!owner) return;
    std::lock_guard<std::mutex> g(g_lock);
    std::vector<Query> still;
    for (Query& q : g_queries)
        if (q.fenceValue || q.owner != owner) still.push_back(q);
    g_queries.swap(still);
}

int Bring(ID3D12CommandQueue* queue, SubmitFn submit, const void* list, UINT segmentDone) {
    if (!queue || !submit) return 0;
    // The queries that have run: in this list's segments the split has
    // waited for, or stamped on a queue whose fence has passed.
    std::vector<Query> ready;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_queries.empty()) return 0;
        std::vector<Query> still;
        for (Query& q : g_queries) {
            bool done = !q.fenceValue && q.owner == list && q.segment <= segmentDone;
            if (!done && q.fenceValue) {
                auto f = g_fences.find(q.queue);
                done = f != g_fences.end() && f->second.fence &&
                       f->second.fence->GetCompletedValue() >= q.fenceValue;
            }
            (done ? ready : still).push_back(q);
        }
        g_queries.swap(still);
    }
    if (ready.empty()) return 0;

    ComPtr<ID3D12Device5> dev;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev)))) return 0;
    const D3D12_COMMAND_LIST_TYPE type = queue->GetDesc().Type;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList4> cl;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev->CreateCommandList(0, type, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        ProxyLog("[dxr-tier-11-proxy-log] deserialized structures: could not create the list "
                 "that decodes them; they stay unknown\n");
        return 0;
    }
    // Each one's size, then its decode and a copy the CPU can read.
    struct Job {
        D3D12_GPU_VIRTUAL_ADDRESS dst;
        astrack::DeserializedAt at;
        UINT64 size;
        ComPtr<ID3D12Resource> out, readback;
    };
    std::vector<Job> jobs;
    for (Query& q : ready) {
        UINT64 size = 0;
        void* p = nullptr;
        D3D12_RANGE r{ q.offset, q.offset + 8 };
        if (SUCCEEDED(q.chunk->readback->Map(0, &r, &p)) && p) {
            std::memcpy(&size, static_cast<const uint8_t*>(p) + q.offset, 8);
            D3D12_RANGE none{ 0, 0 };
            q.chunk->readback->Unmap(0, &none);
        }
        if (!size) {
            ProxyLog("[dxr-tier-11-proxy-log] deserialized structure at 0x%llX: the driver gives "
                     "no decode for tools (size 0); it stays unknown\n", (unsigned long long)q.dst);
            continue;
        }
        Job j{ q.dst, q.at, size, nullptr, nullptr };
        j.out = Buffer(dev.Get(), size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        j.readback = Buffer(dev.Get(), size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (!j.out || !j.readback) continue;
        cl->CopyRaytracingAccelerationStructure(j.out->GetGPUVirtualAddress(), q.dst,
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS);
        jobs.push_back(j);
    }
    if (jobs.empty()) return 0;
    UavBarrier(cl.Get());
    for (Job& j : jobs) {
        Transition(cl.Get(), j.out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyBufferRegion(j.readback.Get(), 0, j.out.Get(), 0, j.size);
    }
    if (FAILED(cl->Close())) return 0;
    ID3D12CommandList* one[] = { cl.Get() };
    submit(queue, 1, one);
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt || FAILED(queue->Signal(fence.Get(), 1))) {
        if (evt) CloseHandle(evt);
        return 0;
    }
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
    }
    CloseHandle(evt);

    int applied = 0;
    for (Job& j : jobs) {
        void* p = nullptr;
        D3D12_RANGE r{ 0, (SIZE_T)j.size };
        std::string what;
        bool ok = false;
        if (SUCCEEDED(j.readback->Map(0, &r, &p)) && p) {
            std::vector<uint8_t> dec(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + j.size);
            // DXR_TIER11_DECODE_POISON=1: a bottom-level structure one geometry
            // short, every instance's contribution one higher. The check that
            // the decode carries the result: gitest --deserialize and raytest
            // --deserialize / --tlasdeser must then diverge.
            static const bool poison = GetEnvironmentVariableA("DXR_TIER11_DECODE_POISON", nullptr, 0) != 0;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_TOOLS_VISUALIZATION_HEADER hdr{};
            if (poison && dec.size() >= sizeof(hdr)) {
                std::memcpy(&hdr, dec.data(), sizeof(hdr));
                if (hdr.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL && hdr.NumDescs > 1) {
                    --hdr.NumDescs;
                    std::memcpy(dec.data(), &hdr, sizeof(hdr));
                } else if (hdr.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
                    for (UINT i = 0; i < hdr.NumDescs; ++i) {
                        const size_t at = sizeof(hdr) + i * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
                        if (at + sizeof(D3D12_RAYTRACING_INSTANCE_DESC) > dec.size()) break;
                        D3D12_RAYTRACING_INSTANCE_DESC x{};
                        std::memcpy(&x, dec.data() + at, sizeof(x));
                        x.InstanceContributionToHitGroupIndex += 1;
                        std::memcpy(dec.data() + at, &x, sizeof(x));
                    }
                }
            }
            ok = astrack::ApplyDecoded(j.dst, j.at, dec.data(), dec.size(), &what);
            D3D12_RANGE none{ 0, 0 };
            j.readback->Unmap(0, &none);
        } else {
            what = "the decode could not be mapped";
        }
        if (ok) ++applied;
        static LONG n = 0;
        if (InterlockedIncrement(&n) <= 16)
            ProxyLog("[dxr-tier-11-proxy-log] deserialized structure at 0x%llX %s: %s\n",
                     (unsigned long long)j.dst, ok ? "DECODED" : "not decoded", what.c_str());
    }
    return applied;
}

}  // namespace asdecode
