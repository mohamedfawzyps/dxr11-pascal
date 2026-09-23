#include "as_tracker.h"
#include "d3d12_command_list.h"
#include "dispatch_stats.h"

#include "proxy_log.h"
#include "res_tracker.h"

#include <wrl/client.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <mutex>

namespace astrack {
namespace {

std::mutex g_lock;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, BlasInfo> g_blas;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, TlasInfo> g_tlas;

// Every top-level build advances this. Per address: the build that last wrote
// the structure, and the build at which its instance data was last asked for.
const UINT64 kLiveWindow = 64;
const UINT64 kRereadEvery = 8;
UINT64 g_tlasSerial = 0;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT64> g_lastBuilt;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT64> g_lastAsked;

// Caller holds g_lock.
bool LiveLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas) {
    auto it = g_lastBuilt.find(tlas);
    if (it == g_lastBuilt.end()) return true;
    return it->second + kLiveWindow >= g_tlasSerial;
}
// Top-level builds since this structure was last rebuilt. A lookup, never
// operator[]: inserting an entry would change what LiveLocked answers.
unsigned long long AgoLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas) {
    auto it = g_lastBuilt.find(tlas);
    return it == g_lastBuilt.end() ? 0ull : g_tlasSerial - it->second;
}
Summary g_summary;

// A recorded copy of a TLAS's instance descriptions, waiting for the GPU.
struct PendingRead {
    D3D12_GPU_VIRTUAL_ADDRESS tlas = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12Resource> keepAlive;   // the copy's own UAV
    UINT   count = 0;
    const void* owner = nullptr;           // the list that recorded the copy
    ID3D12CommandQueue* queue = nullptr;   // set when stamped
    UINT64 fenceValue = 0;                 // 0 until its list is submitted
};
std::vector<PendingRead> g_pending;

// One fence PER QUEUE. A single fence signalled on several queues is not
// ordered: a fast queue can reach value 6 while a slow one has not reached 5,
// and the slow queue's copy would then be read and freed while it runs.
// Single adapter is assumed, which is true of everything this shim targets.
// The queue pointer holds no reference, like the resource tracker, and is
// only compared.
struct QueueFence {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 value = 0;
};
std::map<ID3D12CommandQueue*, QueueFence> g_fences;

// Fills in a TlasInfo from the descriptions themselves. Caller holds g_lock,
// because this reads g_blas to learn what each instance points at.
void ParseLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                 const D3D12_RAYTRACING_INSTANCE_DESC* d, UINT count) {
    TlasInfo t;
    t.valid = true;
    t.instanceCount = count;
// How many records one instance occupies. With the geometry multiplier at 1
    // that is one per geometry in its bottom-level structure, so an instance
    // whose structure holds four geometries covers four consecutive records.
    // A structure never seen being built counts as one, which is what this did
    // before geometry ever entered the index.
    auto geometriesOf = [&](D3D12_GPU_VIRTUAL_ADDRESS blas) -> UINT {
        auto it = g_blas.find(blas);
        if (it == g_blas.end()) return 1;
        return it->second.geometryCount ? it->second.geometryCount : 1;
    };

    for (UINT i = 0; i < count; ++i) {
        const UINT ic = d[i].InstanceContributionToHitGroupIndex;
        if (ic > t.maxContribution) t.maxContribution = ic;
        const UINT end = ic + geometriesOf(d[i].AccelerationStructure);
        if (end > t.recordCount) t.recordCount = end;
    }
    if (t.recordCount == 0) t.recordCount = 1;
    t.reach.assign(t.recordCount, kReachNone);
    t.constants.assign(t.recordCount, RecordConstants{});

    for (UINT i = 0; i < count; ++i) {
        const UINT ic = d[i].InstanceContributionToHitGroupIndex;
        uint8_t bits = kReachNone;
        auto it = g_blas.find(d[i].AccelerationStructure);
        if (it == g_blas.end()) { ++t.unknownBlas; continue; }
        switch (it->second.kind) {
            case Kind::kTriangles:  bits = kReachTriangles; break;
            case Kind::kProcedural: bits = kReachProcedural; break;
            case Kind::kMixed:      bits = kReachTriangles | kReachProcedural; break;
            default:                ++t.unknownBlas; break;
        }
        if (bits & kReachTriangles)  t.anyTriangles = true;
        if (bits & kReachProcedural) t.anyProcedural = true;

        const UINT geoms = geometriesOf(d[i].AccelerationStructure);
        for (UINT gi = 0; gi < geoms; ++gi) {
            const UINT slot = ic + gi;
            if (slot >= t.recordCount) break;
            t.reach[slot] |= bits;

            RecordConstants& rc = t.constants[slot];
            if (rc.assigned) {
                // Same slot, different meaning. Detected, never averaged.
                if (rc.geometryIndex != gi || rc.instanceContribution != ic) {
                    if (!t.constantsConflict) t.conflictSlot = slot;
                    t.constantsConflict = true;
                }
            } else {
                rc.geometryIndex = gi;
                rc.instanceContribution = ic;
                rc.assigned = true;
            }
        }
    }

    auto prev = g_tlas.find(tlas);
    const bool isNew = prev == g_tlas.end();
    if (!isNew && (prev->second.recordCount != t.recordCount ||
                   prev->second.instanceCount != t.instanceCount ||
                   prev->second.maxContribution != t.maxContribution)) {
        static int s_changes = 0;
        if (++s_changes <= 8)
            ProxyLog("[dxr-tier-11-proxy-log] top-level AS at 0x%llX RE-READ, it changed: "
                     "%u -> %u instances, %u -> %u hit group records, max contribution "
                     "%u -> %u.\n",
                     static_cast<unsigned long long>(tlas),
                     prev->second.instanceCount, t.instanceCount,
                     prev->second.recordCount, t.recordCount,
                     prev->second.maxContribution, t.maxContribution);
    }
    if (isNew) dstats::Add(dstats::kTlasNew);
    else if (prev->second.recordCount != t.recordCount ||
             prev->second.instanceCount != t.instanceCount ||
             prev->second.maxContribution != t.maxContribution) dstats::Add(dstats::kTlasChanged);
    else dstats::Add(dstats::kTlasSame);
    g_tlas[tlas] = t;
    if (!isNew) return;

    ++g_summary.tlasRead;
    if (t.maxContribution > g_summary.maxContribution)
        g_summary.maxContribution = t.maxContribution;

    if (g_summary.tlasRead <= 4)
        ProxyLog("[dxr-tier-11-proxy-log] top-level AS at 0x%llX READ: %u instances, max "
                 "InstanceContributionToHitGroupIndex %u, geometry reached: %s%s%s, "
                 "%u instance(s) pointing at an unseen bottom-level structure.\n",
                 static_cast<unsigned long long>(tlas), t.instanceCount,
                 t.maxContribution,
                 t.anyTriangles ? "triangles" : "",
                 (t.anyTriangles && t.anyProcedural) ? " and " : "",
                 t.anyProcedural ? "procedural" : "",
                 t.unknownBlas);

    // The two things the shader table cannot currently express. Said plainly,
    // once each, now that they are measured rather than assumed.
    static bool warnedContribution = false;
    if (t.maxContribution != 0 && !warnedContribution) {
        warnedContribution = true;
        ProxyLog("[dxr-tier-11-proxy-log] NOTE: this scene uses nonzero hit group "
                 "contributions (max %u), so a lowered RayQuery dispatch needs "
                 "%u hit group records rather than one.\n",
                 t.maxContribution, t.maxContribution + 1);
    }
    static bool warnedMixed = false;
    if (t.anyTriangles && t.anyProcedural && !warnedMixed) {
        warnedMixed = true;
        ProxyLog("[dxr-tier-11-proxy-log] NOTE: this scene reaches BOTH triangle and "
                 "procedural geometry from one top-level structure. A lowered "
                 "RayQuery dispatch builds records of one type only, which is "
                 "safe for a triangle-only shader and not for one that commits "
                 "procedural hits.\n");
    }
}

}  // namespace

const char* KindName(Kind k) {
    switch (k) {
        case Kind::kTriangles:  return "triangles";
        case Kind::kProcedural: return "procedural AABBs";
        case Kind::kMixed:      return "MIXED";
        default:                return "unknown";
    }
}

void NoteBuild(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc) {
    if (!desc) return;
    const auto& in = desc->Inputs;

    if (in.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
        // InstanceDescs is a GPU virtual address, and a copy needs a resource.
        // Ask the tracker to name it. Outside the lock: it takes its own.
        const restrack::Found src = restrack::Find(in.InstanceDescs);

        std::lock_guard<std::mutex> g(g_lock);
        ++g_summary.tlasCount;
        g_lastBuilt[desc->DestAccelerationStructureData] = ++g_tlasSerial;
        if (src.resource) ++g_summary.tlasResolved;
        if (!g_summary.sawTopLevel) {
            g_summary.sawTopLevel = true;
            if (src.resource) {
                ProxyLog("[dxr-tier-11-proxy-log] top-level AS build seen, %u instances. "
                         "Instance buffer at 0x%llX RESOLVED to resource %p + "
                         "0x%llX, out of %u tracked buffers, so the "
                         "descriptions can be read from it.\n",
                         in.NumDescs,
                         static_cast<unsigned long long>(in.InstanceDescs),
                         static_cast<void*>(src.resource),
                         static_cast<unsigned long long>(src.offset),
                         static_cast<unsigned>(restrack::Count()));
            } else {
                ProxyLog("[dxr-tier-11-proxy-log] top-level AS build seen, %u instances. "
                         "Instance buffer at 0x%llX did NOT resolve to any of %u "
                         "tracked buffers, so the contributions cannot be "
                         "copied; the table assumes zero.\n",
                         in.NumDescs,
                         static_cast<unsigned long long>(in.InstanceDescs),
                         static_cast<unsigned>(restrack::Count()));
            }
        }
        return;
    }
    if (in.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
        return;

    // ARRAY gives a plain CPU array; ARRAY_OF_POINTERS gives CPU pointers to
    // CPU descs. Both are readable. Anything else is not, and is left unknown
    // rather than guessed at.
    const bool ptrs = in.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
    if (in.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY && !ptrs) return;
    if (!ptrs && !in.pGeometryDescs) return;
    if (ptrs && !in.ppGeometryDescs) return;

    BlasInfo info;
    info.geometryCount = in.NumDescs;
    bool sawTri = false, sawProc = false;
    for (UINT i = 0; i < in.NumDescs; ++i) {
        const D3D12_RAYTRACING_GEOMETRY_DESC* g =
            ptrs ? in.ppGeometryDescs[i] : &in.pGeometryDescs[i];
        if (!g) continue;
        if (g->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS)
            sawProc = true;
        else
            sawTri = true;
    }
    info.kind = (sawTri && sawProc) ? Kind::kMixed
              : sawProc            ? Kind::kProcedural
              : sawTri             ? Kind::kTriangles
                                   : Kind::kUnknown;

    std::lock_guard<std::mutex> g(g_lock);
    const bool isNew = g_blas.find(desc->DestAccelerationStructureData) == g_blas.end();
    g_blas[desc->DestAccelerationStructureData] = info;
    if (!isNew) return;   // a rebuild of the same structure, already counted

    ++g_summary.blasCount;
    switch (info.kind) {
        case Kind::kTriangles:  ++g_summary.triangles; break;
        case Kind::kProcedural: ++g_summary.procedural; break;
        case Kind::kMixed:      ++g_summary.mixed; break;
        default: break;
    }
    // One line per distinct structure. An engine builds a great many, so this
    // is capped; the counts in the summary stay accurate regardless.
    if (g_summary.blasCount <= 8)
        ProxyLog("[dxr-tier-11-proxy-log] bottom-level AS at 0x%llX: %u geometr%s, %s\n",
                 static_cast<unsigned long long>(desc->DestAccelerationStructureData),
                 info.geometryCount, info.geometryCount == 1 ? "y" : "ies",
                 KindName(info.kind));
}

BlasInfo Lookup(D3D12_GPU_VIRTUAL_ADDRESS address) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_blas.find(address);
    return it == g_blas.end() ? BlasInfo() : it->second;
}

void NoteInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                   const D3D12_RAYTRACING_INSTANCE_DESC* descs, UINT count) {
    if (!tlas || !descs || !count) return;
    std::lock_guard<std::mutex> g(g_lock);
    ParseLocked(tlas, descs, count);
}

void NotePendingInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                          ID3D12Resource* readback, UINT count,
                          const void* owner, ID3D12Resource* keepAlive) {
    if (!tlas || !readback || !count || !owner) return;
    PendingRead pr;
    pr.tlas = tlas;
    pr.readback = readback;
    pr.keepAlive = keepAlive;
    pr.count = count;
    pr.owner = owner;
    std::lock_guard<std::mutex> g(g_lock);
    g_pending.push_back(pr);
}

void DropUnsubmitted(const void* owner) {
    if (!owner) return;
    std::lock_guard<std::mutex> g(g_lock);
    if (g_pending.empty()) return;
    std::vector<PendingRead> still;
    for (PendingRead& pr : g_pending)
        if (pr.fenceValue != 0 || pr.owner != owner) still.push_back(pr);
    g_pending.swap(still);
}

namespace {
bool Submitted(const void* owner, ID3D12CommandList* const* lists, UINT count) {
    for (UINT i = 0; i < count; ++i)
        if (static_cast<const void*>(Dxr11CommandList::From(lists[i])) == owner)
            return true;
    return false;
}
}  // namespace

void AfterSubmit(ID3D12CommandQueue* queue,
                 ID3D12CommandList* const* lists, UINT count) {
    if (!queue) return;
    std::lock_guard<std::mutex> g(g_lock);
    if (g_pending.empty()) return;

    // Stamp the reads recorded by the lists in THIS submission, and nothing
    // else. See the header for what stamping an open list's read did.
    bool anyToStamp = false;
    for (const PendingRead& pr : g_pending)
        if (pr.fenceValue == 0 && Submitted(pr.owner, lists, count)) {
            anyToStamp = true;
            break;
        }
    if (anyToStamp) {
        QueueFence& qf = g_fences[queue];
        if (!qf.fence) {
            Microsoft::WRL::ComPtr<ID3D12Device> dev;
            if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev))))
                dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&qf.fence));
        }
        if (qf.fence && SUCCEEDED(queue->Signal(qf.fence.Get(), qf.value + 1))) {
            ++qf.value;
            for (PendingRead& pr : g_pending)
                if (pr.fenceValue == 0 && Submitted(pr.owner, lists, count)) {
                    pr.queue = queue;
                    pr.fenceValue = qf.value;
                }
        }
    }

    // Parse whatever the GPU is already past, judged on the queue each read
    // went out on. Nothing waits.
    std::vector<PendingRead> still;
    for (PendingRead& pr : g_pending) {
        if (pr.fenceValue == 0) { still.push_back(pr); continue; }
        auto it = g_fences.find(pr.queue);
        if (it == g_fences.end() || !it->second.fence ||
            it->second.fence->GetCompletedValue() < pr.fenceValue) {
            still.push_back(pr);
            continue;
        }
        const SIZE_T bytes =
            static_cast<SIZE_T>(pr.count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        void* p = nullptr;
        D3D12_RANGE readRange{ 0, bytes };
        if (SUCCEEDED(pr.readback->Map(0, &readRange, &p)) && p) {
            ParseLocked(pr.tlas,
                        static_cast<const D3D12_RAYTRACING_INSTANCE_DESC*>(p), pr.count);
            D3D12_RANGE noWrite{ 0, 0 };
            pr.readback->Unmap(0, &noWrite);
        } else {
            ProxyLog("[dxr-tier-11-proxy-log] top-level AS at 0x%llX: could not map the "
                     "instance readback buffer.\n",
                     static_cast<unsigned long long>(pr.tlas));
        }
        // Dropped either way; a failed map will not start succeeding.
    }
    g_pending.swap(still);
}

bool WantInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas, UINT numDescs, bool cheap) {
    std::lock_guard<std::mutex> g(g_lock);
    for (const PendingRead& pr : g_pending)
        if (pr.tlas == tlas) return false;   // one copy in flight is enough
    bool want = cheap;
    if (!want) {
        auto it = g_tlas.find(tlas);
        auto asked = g_lastAsked.find(tlas);
        want = it == g_tlas.end() || !it->second.valid ||
               it->second.instanceCount != numDescs ||
               asked == g_lastAsked.end() ||
               asked->second + kRereadEvery <= g_tlasSerial;
    }
    if (want) g_lastAsked[tlas] = g_tlasSerial;
    return want;
}

TlasInfo LookupTlas(D3D12_GPU_VIRTUAL_ADDRESS address) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_tlas.find(address);
    return it == g_tlas.end() ? TlasInfo() : it->second;
}

bool TableWouldBeWrong(bool shaderCommitsProcedural, std::string* why) {
    // Nonzero contributions are not a refusal: the table is sized to the scene
    // and each slot carries a record of the right TYPE, so a hit resolves to a
    // usable record whichever slot the application chose.
    //
    // A triangle-only shader is never refused. Procedural geometry reaches
    // either a null procedural record or, where it shares a slot with
    // triangles, the real triangle record; both produce no hit, which is the
    // same answer the shader gives on Tier 1.1, where it never commits a
    // procedural candidate either.
    // A record that two instances disagree about is refused whatever the
    // shader commits, because the geometry index and the contribution it
    // carries would be wrong for one of them. Checked before the procedural
    // question, since it is not conditional on the shader at all.
    {
        std::lock_guard<std::mutex> g(g_lock);
        std::vector<RecordConstants> merged;
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> from;   // which structure set merged[i]
        for (const auto& kv : g_tlas) {
            const TlasInfo& t = kv.second;
            if (!t.valid || !LiveLocked(kv.first)) continue;
            if (t.constantsConflict) {
                dstats::Add(dstats::kRefusedOneTlas);
                if (why)
                    *why = "two instances put different (contribution, geometry) "
                           "pairs on hit group record " +
                           std::to_string(t.conflictSlot) +
                           ", so that record cannot carry the geometry index for "
                           "both. One record answers once";
                return true;
            }
            // Two LIVE structures disagreeing about a record is the same
            // conflict. Merging them first-read-wins would bake one of them
            // wrongly and say nothing.
            if (merged.size() < t.constants.size()) {
                merged.resize(t.constants.size());
                from.resize(t.constants.size(), 0);
            }
            for (size_t i = 0; i < t.constants.size(); ++i) {
                const RecordConstants& a = t.constants[i];
                RecordConstants& m = merged[i];
                if (!a.assigned) continue;
                if (!m.assigned) { m = a; from[i] = kv.first; continue; }
                if (m.geometryIndex != a.geometryIndex ||
                    m.instanceContribution != a.instanceContribution) {
                    dstats::Add(dstats::kRefusedCrossLive);
                    // Which two, once per pair of addresses: whether they are
                    // one scene double-buffered or two different scenes decides
                    // the fix, and the refusal line alone cannot say.
                    static std::set<std::pair<D3D12_GPU_VIRTUAL_ADDRESS,
                                              D3D12_GPU_VIRTUAL_ADDRESS>> s_seen;
                    const auto key = std::minmax(from[i], kv.first);
                    if (s_seen.size() < 8 && s_seen.insert(key).second) {
                        const TlasInfo& o = g_tlas[from[i]];
                        ProxyLog("[dxr-tier-11-proxy-log] conflict: record %u is (contribution "
                                 "%u, geometry %u) in 0x%llX (%u instances, %u records, "
                                 "built %llu top-level builds ago) and (%u, %u) in 0x%llX "
                                 "(%u instances, %u records, built %llu ago)\n",
                                 static_cast<unsigned>(i),
                                 m.instanceContribution, m.geometryIndex,
                                 static_cast<unsigned long long>(from[i]),
                                 o.instanceCount, o.recordCount,
                                 AgoLocked(from[i]),
                                 a.instanceContribution, a.geometryIndex,
                                 static_cast<unsigned long long>(kv.first),
                                 t.instanceCount, t.recordCount,
                                 AgoLocked(kv.first));
                    }
                    if (why)
                        *why = "two live top-level structures put different "
                               "(contribution, geometry) pairs on hit group record " +
                               std::to_string(i) + ". One record answers once";
                    return true;
                }
            }
        }
    }

    if (!shaderCommitsProcedural) return false;

    // For a shader that DOES commit procedural hits, one slot holding both
    // kinds is the end of it: that slot would need a procedural record for the
    // procedural geometry and a rejecting triangle record for the triangles,
    // and a record is one or the other. The application collapsed them and
    // nothing here can undo that.
    std::lock_guard<std::mutex> g(g_lock);
    for (const auto& kv : g_tlas) {
        const TlasInfo& t = kv.second;
        if (!t.valid || !LiveLocked(kv.first)) continue;
        for (size_t i = 0; i < t.reach.size(); ++i) {
            if ((t.reach[i] & kReachTriangles) && (t.reach[i] & kReachProcedural)) {
                dstats::Add(dstats::kRefusedProcedural);
                if (why)
                    *why = "this shader commits procedural hits, and the scene "
                           "routes BOTH triangle and procedural geometry to the "
                           "same hit group record, which can only be one of the "
                           "two";
                return true;
            }
        }
    }
    return false;
}

std::vector<RecordConstants> RecordConstantsTable() {
    std::lock_guard<std::mutex> g(g_lock);
    std::vector<RecordConstants> out;
    for (const auto& kv : g_tlas) {
        const TlasInfo& t = kv.second;
        if (!t.valid || !LiveLocked(kv.first)) continue;
        if (out.size() < t.constants.size()) out.resize(t.constants.size());
        for (size_t i = 0; i < t.constants.size(); ++i)
            if (t.constants[i].assigned && !out[i].assigned) out[i] = t.constants[i];
    }
    return out;
}

std::vector<uint8_t> RecordKinds() {
    std::lock_guard<std::mutex> g(g_lock);
    std::vector<uint8_t> out;
    for (const auto& kv : g_tlas) {
        const TlasInfo& t = kv.second;
        if (!t.valid || !LiveLocked(kv.first)) continue;
        if (out.size() < t.reach.size()) out.resize(t.reach.size(), kReachNone);
        for (size_t i = 0; i < t.reach.size(); ++i) out[i] |= t.reach[i];
    }
    return out;
}

std::string DescribeLive() {
    std::lock_guard<std::mutex> g(g_lock);
    std::string out;
    int n = 0;
    for (const auto& kv : g_tlas) {
        if (!kv.second.valid || !LiveLocked(kv.first)) continue;
        if (++n > 8) { out += " ..."; break; }
        char buf[160];
        snprintf(buf, sizeof(buf), "%s0x%llX %u inst %u rec built %llu ago",
                 out.empty() ? "" : ", ",
                 static_cast<unsigned long long>(kv.first),
                 kv.second.instanceCount, kv.second.recordCount,
                 AgoLocked(kv.first));
        out += buf;
    }
    return out;
}

Summary GetSummary() {
    std::lock_guard<std::mutex> g(g_lock);
    return g_summary;
}

}  // namespace astrack
