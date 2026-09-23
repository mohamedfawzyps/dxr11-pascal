#include "gpu_hold.h"

#include "d3d12_command_list.h"

#include <windows.h>
#include <wrl/client.h>

#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gpuhold {
namespace {

struct Stamp {
    const void* res = nullptr;
    ID3D12CommandQueue* queue = nullptr;   // compared only, no reference
    UINT64 fence = 0;                      // 0: never submitted
};

// One fence per queue, for the reason as_tracker gives: a fence signalled on
// several queues is not ordered.
struct QueueFence {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
};

// Allocated once and NEVER destroyed. As a static it would be destroyed when
// this DLL unloads at process exit, releasing fences after D3D12 itself may
// already be gone: that crashed raytest at exit with 0xC0000409, on about
// half of all runs, depending on the order the loader unloaded DLLs in.
struct State {
    std::mutex lock;
    std::unordered_map<const void*, std::vector<Stamp>> live;   // owner -> uses
    std::vector<Stamp> ending;   // owner gone, waiting for the fence
    std::map<ID3D12CommandQueue*, QueueFence> fences;
};
State& St() {
    static State* s = new State;
    return *s;
}

// Caller holds St().lock. Signals a new value on `queue`, 0 on failure.
UINT64 SignalLocked(ID3D12CommandQueue* queue) {
    QueueFence& qf = St().fences[queue];
    if (!qf.fence) {
        Microsoft::WRL::ComPtr<ID3D12Device> dev;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&qf.fence))))
            return 0;
    }
    if (FAILED(queue->Signal(qf.fence.Get(), qf.value + 1))) return 0;
    return ++qf.value;
}

// Caller holds St().lock.
bool DoneLocked(const Stamp& s) {
    auto it = St().fences.find(s.queue);
    return it != St().fences.end() && it->second.fence &&
           it->second.fence->GetCompletedValue() >= s.fence;
}

// Caller holds St().lock. A re-execution on another queue while the first is
// still running keeps both stamps; otherwise the newer one replaces it.
void StampLocked(std::vector<Stamp>& uses, ID3D12CommandQueue* queue, UINT64 value) {
    std::vector<Stamp> extra;
    for (Stamp& s : uses) {
        if (s.fence && s.queue != queue && !DoneLocked(s)) extra.push_back(s);
        s.queue = queue;
        s.fence = value;
    }
    uses.insert(uses.end(), extra.begin(), extra.end());
}

// Caller holds St().lock.
void EndLocked(const void* owner) {
    auto it = St().live.find(owner);
    if (it == St().live.end()) return;
    for (const Stamp& s : it->second)
        if (s.fence && !DoneLocked(s)) St().ending.push_back(s);
    St().live.erase(it);
}

// Caller holds St().lock.
void SweepLocked() {
    std::vector<Stamp> still;
    for (const Stamp& s : St().ending)
        if (!DoneLocked(s)) still.push_back(s);
    St().ending.swap(still);
}

}  // namespace

void Use(const void* res, const void* owner) {
    if (!res || !owner) return;
    std::lock_guard<std::mutex> g(St().lock);
    std::vector<Stamp>& uses = St().live[owner];
    for (const Stamp& s : uses)
        if (s.res == res) return;
    Stamp s;
    s.res = res;
    uses.push_back(s);
}

void AfterSubmit(ID3D12CommandQueue* queue, ID3D12CommandList* const* lists, UINT count) {
    if (!queue) return;
    {
        std::lock_guard<std::mutex> g(St().lock);
        if (St().live.empty() && St().ending.empty()) return;
    }
    // Resolved outside the lock: From is a QueryInterface on the list.
    std::vector<const void*> owners;
    for (UINT i = 0; i < count; ++i)
        if (const void* w = Dxr11CommandList::From(lists[i])) owners.push_back(w);

    std::lock_guard<std::mutex> g(St().lock);
    bool any = false;
    for (const void* o : owners) any |= St().live.count(o) != 0;
    if (any) {
        const UINT64 value = SignalLocked(queue);
        if (value)
            for (const void* o : owners) {
                auto it = St().live.find(o);
                if (it != St().live.end()) StampLocked(it->second, queue, value);
            }
    }
    SweepLocked();
}

void SubmittedOnce(ID3D12CommandQueue* queue, const void* owner) {
    if (!queue || !owner) return;
    std::lock_guard<std::mutex> g(St().lock);
    auto it = St().live.find(owner);
    if (it == St().live.end()) return;
    const UINT64 value = SignalLocked(queue);
    if (value) {
        StampLocked(it->second, queue, value);
        EndLocked(owner);
    }
    // A failed signal leaves the uses live, so the buffer stays busy: leaking
    // one table is the safe side of not knowing.
}

void Detach(const void* owner) {
    if (!owner) return;
    std::lock_guard<std::mutex> g(St().lock);
    EndLocked(owner);
}

bool Busy(const void* res) {
    if (!res) return false;
    // DXR_TIER11_NOHOLD=1 answers "idle" always, so evicted tables are reused
    // while still in flight. TEST ONLY: it is how `churn` shows it can fail.
    static const bool noHold = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("DXR_TIER11_NOHOLD", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    if (noHold) return false;
    std::lock_guard<std::mutex> g(St().lock);
    for (const auto& kv : St().live)
        for (const Stamp& s : kv.second)
            if (s.res == res) return true;
    for (const Stamp& s : St().ending)
        if (s.res == res && !DoneLocked(s)) return true;
    return false;
}

}  // namespace gpuhold
