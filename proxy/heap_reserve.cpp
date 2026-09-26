// See the header.
#include "heap_reserve.h"

#include "gpu_hold.h"
#include "proxy_log.h"

#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace heapres {
namespace {

const UINT kReserve = 4096;          // descriptors at the end of each heap
const UINT kMaxHeap = 1000000;       // what every device takes (Tier 1 and 2)

// One range in a reserve. Its address is the key gpu_hold tracks its uses by.
struct Entry {
    UINT at = 0, n = 0;
    std::vector<View> views;
};

struct Reserve {
    ID3D12DescriptorHeap* heap = nullptr;   // no reference: the application's
    UINT base = 0, size = 0;                // the reserve: [base, base + size)
    SIZE_T cpu0 = 0;
    UINT64 gpu0 = 0;
    UINT inc = 0;
    std::vector<std::unique_ptr<Entry>> entries;   // sorted by `at`
    std::map<std::vector<View>, Entry*> byViews;
};

typedef D3D12_DESCRIPTOR_HEAP_DESC*(STDMETHODCALLTYPE* GetDescFn)(ID3D12DescriptorHeap*,
                                                                  D3D12_DESCRIPTOR_HEAP_DESC*);

// Never destroyed, for the reason gpu_hold gives.
struct State {
    std::mutex lock;
    std::unordered_map<ID3D12DescriptorHeap*, std::shared_ptr<Reserve>> heaps;
    SRWLOCK hookLock = SRWLOCK_INIT;
    std::unordered_map<void**, GetDescFn> originals;   // vtable -> its GetDesc
    std::unordered_map<ID3D12DescriptorHeap*, UINT> asked;   // heap -> size asked for
    ComPtr<ID3D12DescriptorHeap> own;                   // for a dispatch with none bound
    ID3D12Device* ownDevice = nullptr;
};
State& St() {
    static State* s = new State;
    return *s;
}

// {5E2C1A7B-93D4-4F0E-B6A1-2C8D7E4F9A30}
const GUID kHolderGuid = { 0x5e2c1a7b, 0x93d4, 0x4f0e, { 0xb6, 0xa1, 0x2c, 0x8d, 0x7e, 0x4f, 0x9a, 0x30 } };

D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE HookGetDesc(ID3D12DescriptorHeap* self,
                                                           D3D12_DESCRIPTOR_HEAP_DESC* ret) {
    void** vt = *reinterpret_cast<void***>(self);
    GetDescFn orig = nullptr;
    UINT asked = 0;
    bool grown = false;
    AcquireSRWLockShared(&St().hookLock);
    auto o = St().originals.find(vt);
    if (o != St().originals.end()) orig = o->second;
    auto a = St().asked.find(self);
    if (a != St().asked.end()) { asked = a->second; grown = true; }
    ReleaseSRWLockShared(&St().hookLock);
    if (!orig) { std::memset(ret, 0, sizeof(*ret)); return ret; }   // cannot happen
    orig(self, ret);
    if (grown) ret->NumDescriptors = asked;
    return ret;
}

// Hooks GetDesc on `h`'s vtable, once per vtable.
bool Hook(ID3D12DescriptorHeap* h) {
    void** vt = *reinterpret_cast<void***>(h);
    AcquireSRWLockExclusive(&St().hookLock);
    bool ok = St().originals.count(vt) != 0;
    if (!ok) {
        DWORD old = 0;
        if (VirtualProtect(&vt[8], sizeof(void*), PAGE_READWRITE, &old)) {
            if (vt[8] != reinterpret_cast<void*>(&HookGetDesc)) {
                St().originals[vt] = reinterpret_cast<GetDescFn>(vt[8]);
                vt[8] = reinterpret_cast<void*>(&HookGetDesc);
                ok = true;
            }
            DWORD ignored = 0;
            VirtualProtect(&vt[8], sizeof(void*), old, &ignored);
        }
    }
    ReleaseSRWLockExclusive(&St().hookLock);
    return ok;
}

// Forgets a grown heap when the application releases it.
class Holder : public IUnknown {
public:
    explicit Holder(ID3D12DescriptorHeap* h) : m_heap(h) {}
    ~Holder() {
        AcquireSRWLockExclusive(&St().hookLock);
        St().asked.erase(m_heap);
        ReleaseSRWLockExclusive(&St().hookLock);
        std::lock_guard<std::mutex> g(St().lock);
        St().heaps.erase(m_heap);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (riid == __uuidof(IUnknown)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&m_refs);
        if (!r) delete this;
        return r;
    }
private:
    LONG m_refs = 1;
    ID3D12DescriptorHeap* m_heap;
};

std::shared_ptr<Reserve> MakeReserve(ID3D12Device* real, ID3D12DescriptorHeap* h, UINT base, UINT size) {
    auto r = std::make_shared<Reserve>();
    r->heap = h;
    r->base = base;
    r->size = size;
    r->cpu0 = h->GetCPUDescriptorHandleForHeapStart().ptr;
    r->gpu0 = h->GetGPUDescriptorHandleForHeapStart().ptr;
    r->inc = real->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return r;
}

bool RangePoison() {
    static const bool p = GetEnvironmentVariableA("DXR_TIER11_RANGE_POISON", nullptr, 0) != 0;
    return p;
}

// Caller holds St().lock. A free run of `n` in `r`, first fit; when there is
// none, every range no work can read any more is dropped and it looks again.
// -1 when there is still none.
int Place(Reserve& r, UINT n) {
    for (int pass = 0; pass < 2; ++pass) {
        UINT at = 0;
        size_t i = 0;
        for (; i <= r.entries.size(); ++i) {
            const UINT end = i < r.entries.size() ? r.entries[i]->at : r.size;
            if (end >= at + n) return (int)at;
            if (i < r.entries.size()) at = r.entries[i]->at + r.entries[i]->n;
        }
        if (pass) break;
        // Drop everything idle, then look again.
        std::vector<Entry*> idle;
        for (auto& e : r.entries)
            if (!gpuhold::Busy(e.get())) idle.push_back(e.get());
        if (idle.empty()) break;
        for (Entry* e : idle) r.byViews.erase(e->views);
        r.entries.erase(std::remove_if(r.entries.begin(), r.entries.end(),
                                       [&](const std::unique_ptr<Entry>& e) {
                                           return std::find(idle.begin(), idle.end(), e.get()) != idle.end();
                                       }),
                        r.entries.end());
    }
    return -1;
}

}  // namespace

unsigned Forced() {
    static const unsigned f = [] {
        char v[4] = {};
        if (!GetEnvironmentVariableA("DXR_TIER11_LRS_TABLE", v, sizeof(v))) return 0u;
        return v[0] == '2' ? 2u : 1u;
    }();
    return f;
}

HRESULT Create(ID3D12Device* real, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out) {
    if (!desc || !out || desc->Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        !(desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) || desc->NumDescriptors >= kMaxHeap)
        return real->CreateDescriptorHeap(desc, riid, out);
    const UINT extra = (std::min)(kReserve, kMaxHeap - desc->NumDescriptors);
    if (extra < 256) return real->CreateDescriptorHeap(desc, riid, out);
    D3D12_DESCRIPTOR_HEAP_DESC grown = *desc;
    grown.NumDescriptors += extra;
    ComPtr<ID3D12DescriptorHeap> h;
    if (FAILED(real->CreateDescriptorHeap(&grown, IID_PPV_ARGS(&h))) || !h)
        return real->CreateDescriptorHeap(desc, riid, out);
    // GetDesc must report what was asked before anything else sees the heap;
    // if it cannot, the reserve is left unused (the application then sees a
    // larger heap, which it may use: so the shim must not).
    bool hooked = Hook(h.Get());
    if (hooked) {
        AcquireSRWLockExclusive(&St().hookLock);
        St().asked[h.Get()] = desc->NumDescriptors;
        ReleaseSRWLockExclusive(&St().hookLock);
        hooked = h->GetDesc().NumDescriptors == desc->NumDescriptors;
        if (!hooked) {
            AcquireSRWLockExclusive(&St().hookLock);
            St().asked.erase(h.Get());
            ReleaseSRWLockExclusive(&St().hookLock);
        }
    }
    if (hooked) {
        Holder* holder = new Holder(h.Get());
        h->SetPrivateDataInterface(kHolderGuid, holder);
        holder->Release();
        std::lock_guard<std::mutex> g(St().lock);
        St().heaps[h.Get()] = MakeReserve(real, h.Get(), desc->NumDescriptors, extra);
    }
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] shader-visible heaps get %u descriptors of the shim's at "
                 "their end, for the shim's own layouts past the local root signature limit%s\n",
                 extra, hooked ? "" : " (NOT: GetDesc could not be hooked, so none is used)");
    return h->QueryInterface(riid, out);
}

bool Range(ID3D12Device* real, const std::vector<ID3D12DescriptorHeap*>& heaps,
           const std::vector<View>& views, const void* owner, D3D12_GPU_DESCRIPTOR_HANDLE* out,
           std::vector<ID3D12DescriptorHeap*>* bind, std::string* why) {
    bind->clear();
    if (views.empty()) { *why = "an empty descriptor range"; return false; }
    // The bound CBV/SRV/UAV heap, if any. Its type from the real GetDesc path
    // (through the hook, which changes only the count).
    ID3D12DescriptorHeap* view = nullptr;
    for (auto* h : heaps)
        if (h && h->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) view = h;
    std::lock_guard<std::mutex> g(St().lock);
    std::shared_ptr<Reserve> r;
    if (view) {
        auto it = St().heaps.find(view);
        if (it == St().heaps.end()) {
            *why = "the descriptor heap bound at the dispatch has no room for the shim's descriptors "
                   "(it is already the largest every device takes, 1,000,000)";
            return false;
        }
        r = it->second;
    } else {
        // Nothing bound: the shim's own heap, all of it reserve. Binding it
        // cannot disturb the application, which reads no table.
        if (!St().own || St().ownDevice != real) {
            D3D12_DESCRIPTOR_HEAP_DESC d{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kReserve,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
            ComPtr<ID3D12DescriptorHeap> h;
            if (FAILED(real->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h)))) {
                *why = "could not create the shim's descriptor heap";
                return false;
            }
            St().own = h;
            St().ownDevice = real;
            St().heaps[h.Get()] = MakeReserve(real, h.Get(), 0, kReserve);
        }
        r = St().heaps[St().own.Get()];
        *bind = heaps;
        bind->push_back(St().own.Get());
    }
    Entry* e = nullptr;
    auto hit = r->byViews.find(views);
    if (hit != r->byViews.end()) {
        e = hit->second;
    } else {
        const UINT n = (UINT)views.size();
        if (n > r->size) { *why = "more descriptors than the shim's reserve holds"; return false; }
        const int at = Place(*r, n);
        if (at < 0) {
            *why = "the shim's descriptor reserve is full of ranges still in flight";
            return false;
        }
        auto ne = std::make_unique<Entry>();
        ne->at = (UINT)at;
        ne->n = n;
        ne->views = views;
        for (UINT i = 0; i < n; ++i) {
            const View& v = views[RangePoison() ? 0 : i];
            D3D12_CPU_DESCRIPTOR_HANDLE c{ r->cpu0 + (SIZE_T)(r->base + at + i) * r->inc };
            D3D12_SHADER_RESOURCE_VIEW_DESC s{};
            s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (v.raw) {
                s.Format = DXGI_FORMAT_R32_TYPELESS;
                s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                s.Buffer.NumElements = (UINT)(v.bytes / 4);
                s.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                real->CreateShaderResourceView(v.raw, &s, c);
            } else {
                s.Format = DXGI_FORMAT_UNKNOWN;
                s.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                s.RaytracingAccelerationStructure.Location = v.as;
                real->CreateShaderResourceView(nullptr, &s, c);
            }
        }
        e = ne.get();
        auto pos = std::lower_bound(r->entries.begin(), r->entries.end(), ne,
                                    [](const std::unique_ptr<Entry>& a, const std::unique_ptr<Entry>& b) {
                                        return a->at < b->at;
                                    });
        r->entries.insert(pos, std::move(ne));
        r->byViews[views] = e;
    }
    gpuhold::Use(e, owner);
    out->ptr = r->gpu0 + (UINT64)(r->base + e->at) * r->inc;
    return true;
}

}  // namespace heapres
