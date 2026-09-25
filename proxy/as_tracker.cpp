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
// Every acceleration structure build advances this, in the order recorded.
// Per bottom-level address, the builds it has had, oldest first; per
// top-level address, the serial of its latest build. A top-level build's
// instances are parsed against the bottom-level structures as they were when
// it was RECORDED: since 0.52.0 GPU-written ones are parsed at submit, when
// an engine streaming geometry has often recorded the next frame already and
// reused an address for a structure with a different geometry count. Parsed
// against the latest, the table was wrong, silently (0.52.3).
UINT64 g_asSerial = 0;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, std::vector<std::pair<UINT64, BlasInfo>>> g_blasHist;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT64> g_tlasAsSerial;
// A top-level structure made by CLONE or COMPACT of another whose read was
// still pending: build `build` of `dst` is build `srcBuild` of `src`.
struct Alias { D3D12_GPU_VIRTUAL_ADDRESS src = 0; UINT64 srcBuild = 0, build = 0; };
std::map<D3D12_GPU_VIRTUAL_ADDRESS, Alias> g_alias;

// Caller holds g_lock. The bottom-level structure at `blas` as the build
// recorded at `asOf` saw it: its latest build recorded before that. Null
// when none was.
const BlasInfo* BlasAtLocked(D3D12_GPU_VIRTUAL_ADDRESS blas, UINT64 asOf) {
    auto h = g_blasHist.find(blas);
    if (h == g_blasHist.end()) return nullptr;
    const BlasInfo* found = nullptr;
    for (const auto& e : h->second)
        if (e.first < asOf) found = &e.second;
    return found;
}
std::map<D3D12_GPU_VIRTUAL_ADDRESS, TlasInfo> g_tlas;

// Every top-level build advances this. Per address: the build that last wrote
// the structure, and the build at which its instance data was last asked for.
const UINT64 kLiveWindow = 64;
const UINT64 kRereadEvery = 8;
UINT64 g_tlasSerial = 0;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT64> g_lastBuilt;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT64> g_lastAsked;
// Per address: the build that FIRST wrote it, and how many builds have.
std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT64> g_firstBuilt;
std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT64> g_buildCount;
// Per address: the build (a g_lastBuilt value) the instance data in g_tlas
// was read from. A GPU-written structure is read again only every few builds
// and a submission late, so what is known can be an OLDER build's scene; a
// table built from it can put the wrong geometry on a record (0.51.0).
std::map<D3D12_GPU_VIRTUAL_ADDRESS, UINT64> g_readOf;

// Caller holds g_lock. Was the structure's instance data read from its
// latest build?
bool CurrentLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas) {
    auto b = g_lastBuilt.find(tlas);
    auto r = g_readOf.find(tlas);
    return b != g_lastBuilt.end() && r != g_readOf.end() && r->second == b->second;
}

// A structure is SUPERSEDED once another one that did not exist at its last
// build has been built this many times without it being rebuilt. That is an
// engine moving its scene to a new buffer: Escher did it twice a session, and
// for the ~64 builds until the old one aged out, both counted, disagreed
// about records, and every lowered dispatch was refused. Two structures an
// engine keeps alive together are both rebuilt, so neither supersedes the
// other. The cost: a structure built once and traced forever stops counting
// after a newer one has four builds, where the window alone gave it 64.
const UINT64 kSupersedeBuilds = 4;

bool LiveLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas);

// Caller holds g_lock. The structures a table is judged over: exactly `only`
// when the dispatch's scene is known, every live one otherwise.
bool CountsLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas, const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* only) {
    return only ? std::find(only->begin(), only->end(), tlas) != only->end() : LiveLocked(tlas);
}

// Caller holds g_lock.
bool LiveLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas) {
    auto it = g_lastBuilt.find(tlas);
    if (it == g_lastBuilt.end()) return true;
    if (it->second + kLiveWindow < g_tlasSerial) return false;
    // Only structures whose instance data has been read can supersede: one
    // the shim knows nothing about must not take a known scene's place.
    for (const auto& kv : g_tlas) {
        if (kv.first == tlas || !kv.second.valid) continue;
        auto first = g_firstBuilt.find(kv.first);
        auto count = g_buildCount.find(kv.first);
        if (first != g_firstBuilt.end() && count != g_buildCount.end() &&
            first->second > it->second && count->second >= kSupersedeBuilds)
            return false;
    }
    return true;
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
    UINT64 build = 0;                      // the build it copies, see g_readOf
    UINT64 asOf = 0;                       // that build's g_asSerial
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
// because this reads the bottom-level history to learn what each instance
// points at, as of `asOf`, the serial of the top-level build (0: now).
void ParseLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                 const D3D12_RAYTRACING_INSTANCE_DESC* d, UINT count, UINT64 asOf) {
    if (!asOf) asOf = g_asSerial + 1;
    TlasInfo t;
    t.valid = true;
    t.instanceCount = count;
// How many records one instance occupies. With the geometry multiplier at 1
    // that is one per geometry in its bottom-level structure, so an instance
    // whose structure holds four geometries covers four consecutive records.
    // A structure never seen being built counts as one, which is what this did
    // before geometry ever entered the index.
    auto geometriesOf = [&](D3D12_GPU_VIRTUAL_ADDRESS blas) -> UINT {
        const BlasInfo* b = BlasAtLocked(blas, asOf);
        if (!b) return 1;
        return b->geometryCount ? b->geometryCount : 1;
    };

    // An instance with a NULL bottom-level structure is legal but INACTIVE,
    // discarded at build (spec, "Inactive primitives and instances"): it
    // reaches no record. Unreal writes every culled instance that way. Until
    // 0.53.1 it counted as an unknown structure, which 0.53.0 refused.
    for (UINT i = 0; i < count; ++i) {
        if (!d[i].AccelerationStructure) continue;
        const UINT ic = d[i].InstanceContributionToHitGroupIndex;
        if (ic > t.maxContribution) t.maxContribution = ic;
        const UINT end = ic + geometriesOf(d[i].AccelerationStructure);
        if (end > t.recordCount) t.recordCount = end;
    }
    if (t.recordCount == 0) t.recordCount = 1;
    t.reach.assign(t.recordCount, kReachNone);
    t.constants.assign(t.recordCount, RecordConstants{});

    auto noteUnknown = [&](UINT i, const BlasInfo* bi) {
        if (t.unknownBlas++) return;
        const D3D12_GPU_VIRTUAL_ADDRESS a = d[i].AccelerationStructure;
        auto h = g_blasHist.find(a);
        char buf[400];
        if (bi) {
            static const char* kOrigin[] = { "built with no readable geometry",
                "a copy of a structure of unknown geometry", "DESERIALIZED",
                "a copy of something never seen" };
            std::snprintf(buf, sizeof(buf), " (first: instance %u of %u, structure 0x%llX, %s, "
                          "%u geometries)", i, count, (unsigned long long)a,
                          kOrigin[bi->origin < 4 ? bi->origin : 0], bi->geometryCount);
        } else if (h == g_blasHist.end() || h->second.empty()) {
            std::snprintf(buf, sizeof(buf), " (first: instance %u of %u, structure 0x%llX, never "
                          "seen built or copied)", i, count, (unsigned long long)a);
        } else {
            std::snprintf(buf, sizeof(buf), " (first: instance %u of %u, structure 0x%llX, its "
                          "first build of %zu remembered was RECORDED after this top-level "
                          "build: serial %llu, top-level at %llu)", i, count,
                          (unsigned long long)a, h->second.size(),
                          (unsigned long long)h->second.front().first,
                          (unsigned long long)asOf);
        }
        t.unknownDetail = buf;
    };
    for (UINT i = 0; i < count; ++i) {
        if (!d[i].AccelerationStructure) continue;   // inactive
        const UINT ic = d[i].InstanceContributionToHitGroupIndex;
        uint8_t bits = kReachNone;
        const BlasInfo* bi = BlasAtLocked(d[i].AccelerationStructure, asOf);
        if (!bi) { noteUnknown(i, nullptr); continue; }
        switch (bi->kind) {
            case Kind::kTriangles:  bits = kReachTriangles; break;
            case Kind::kProcedural: bits = kReachProcedural; break;
            case Kind::kMixed:      bits = kReachTriangles | kReachProcedural; break;
            default:                noteUnknown(i, bi); break;
        }
        if (bits & kReachTriangles)  t.anyTriangles = true;
        if (bits & kReachProcedural) t.anyProcedural = true;

        const UINT geoms = geometriesOf(d[i].AccelerationStructure);
        t.classes.emplace_back(ic, geoms);
        for (UINT gi = 0; gi < geoms; ++gi) {
            const UINT slot = ic + gi;
            if (slot >= t.recordCount) break;
            t.reach[slot] |= bits;

            RecordConstants& rc = t.constants[slot];
            if (rc.assigned) {
                // Same slot, different meaning. Detected, never averaged.
                if (rc.geometryIndex != gi || rc.instanceContribution != ic) {
                    if (!t.constantsConflict) {
                        t.conflictSlot = slot;
                        const BlasInfo* bi2 = BlasAtLocked(d[i].AccelerationStructure, asOf);
                        auto h = g_blasHist.find(d[i].AccelerationStructure);
                        char buf[400];
                        std::snprintf(buf, sizeof(buf),
                            " (already: contribution %u geometry %u; then instance %u of %u, "
                            "contribution %u geometry %u, structure 0x%llX with %u geometries "
                            "as of this build, %zu build(s) of that address remembered)",
                            rc.instanceContribution, rc.geometryIndex, i, count, ic, gi,
                            (unsigned long long)d[i].AccelerationStructure,
                            bi2 ? bi2->geometryCount : 0u,
                            h == g_blasHist.end() ? (size_t)0 : h->second.size());
                        t.conflictDetail = buf;
                    }
                    t.constantsConflict = true;
                }
            } else {
                rc.geometryIndex = gi;
                rc.instanceContribution = ic;
                rc.assigned = true;
            }
        }
    }

    std::sort(t.classes.begin(), t.classes.end());
    t.classes.erase(std::unique(t.classes.begin(), t.classes.end()), t.classes.end());

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

const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* NoDuplicateAnyHit(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* in,
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* copy,
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>* storage) {
    if (!in || in->Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL ||
        !in->NumDescs)
        return in;
    const bool ptrs = in->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
    if (ptrs ? !in->ppGeometryDescs : !in->pGeometryDescs) return in;
    auto at = [&](UINT i) -> const D3D12_RAYTRACING_GEOMETRY_DESC& {
        return ptrs ? *in->ppGeometryDescs[i] : in->pGeometryDescs[i];
    };
    const auto kFlag = D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
    bool all = true;
    for (UINT i = 0; i < in->NumDescs && all; ++i)
        all = (at(i).Flags & kFlag) != 0;
    if (all) return in;

    storage->assign(in->NumDescs, D3D12_RAYTRACING_GEOMETRY_DESC{});
    for (UINT i = 0; i < in->NumDescs; ++i) {
        (*storage)[i] = at(i);
        (*storage)[i].Flags |= kFlag;
    }
    *copy = *in;
    copy->DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    copy->pGeometryDescs = storage->data();

    static LONG s_said = 0;
    if (InterlockedCompareExchange(&s_said, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] setting NO_DUPLICATE_ANYHIT_INVOCATION on "
                 "bottom-level geometry (first build: %u geometries), so an any-hit "
                 "shader runs once per intersection, as a Proceed loop sees each "
                 "candidate once. Applied in the prebuild query too.\n",
                 in->NumDescs);
    return copy;
}

void NoteBuild(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* desc) {
    if (!desc) return;
    const auto& in = desc->Inputs;

    if (in.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
        // InstanceDescs is a GPU virtual address, and a copy needs a resource.
        // Ask the tracker to name it. Outside the lock: it takes its own.
        const restrack::Found src = restrack::Find(in.InstanceDescs);

        std::lock_guard<std::mutex> g(g_lock);
        g_tlasAsSerial[desc->DestAccelerationStructureData] = ++g_asSerial;
        ++g_summary.tlasCount;
        g_lastBuilt[desc->DestAccelerationStructureData] = ++g_tlasSerial;
        g_firstBuilt.emplace(desc->DestAccelerationStructureData, g_tlasSerial);
        ++g_buildCount[desc->DestAccelerationStructureData];
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
                         "Instance buffer at 0x%llX is not in any of %u "
                         "tracked buffers; its descriptions are read back by "
                         "address on the GPU.\n",
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
    // The history: an entry older than every top-level build a pending read
    // may still be parsed as of can go, but the latest before that stays.
    {
        UINT64 floor = g_asSerial + 1;
        for (const PendingRead& pr : g_pending)
            if (pr.asOf && pr.asOf < floor) floor = pr.asOf;
        for (const auto& kv : g_tlasAsSerial)
            if (kv.second < floor && !CurrentLocked(kv.first)) floor = kv.second;
        auto& h = g_blasHist[desc->DestAccelerationStructureData];
        h.emplace_back(++g_asSerial, info);
        size_t keepFrom = 0;
        for (size_t i = 0; i + 1 < h.size(); ++i)
            if (h[i + 1].first < floor) keepFrom = i + 1;
        if (keepFrom) h.erase(h.begin(), h.begin() + keepFrom);
    }
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

std::map<D3D12_GPU_VIRTUAL_ADDRESS, std::vector<D3D12_RAYTRACING_INSTANCE_DESC>> g_snapshots;

void NoteInstances(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                   const D3D12_RAYTRACING_INSTANCE_DESC* descs, UINT count) {
    if (!tlas || !descs || !count) return;
    std::lock_guard<std::mutex> g(g_lock);
    ParseLocked(tlas, descs, count, g_tlasAsSerial[tlas]);
    auto b = g_lastBuilt.find(tlas);
    if (b != g_lastBuilt.end()) g_readOf[tlas] = b->second;
    g_snapshots[tlas].assign(descs, descs + count);
}

void NoteEmpty(D3D12_GPU_VIRTUAL_ADDRESS tlas) {
    if (!tlas) return;
    std::lock_guard<std::mutex> g(g_lock);
    ParseLocked(tlas, nullptr, 0, 0);
    auto b = g_lastBuilt.find(tlas);
    if (b != g_lastBuilt.end()) g_readOf[tlas] = b->second;
    g_snapshots[tlas].clear();
}

bool InstanceSnapshot(D3D12_GPU_VIRTUAL_ADDRESS tlas,
                      std::vector<D3D12_RAYTRACING_INSTANCE_DESC>* out) {
    std::lock_guard<std::mutex> g(g_lock);
    auto it = g_snapshots.find(tlas);
    if (it == g_snapshots.end()) return false;
    *out = it->second;
    return true;
}

void DropSnapshot(D3D12_GPU_VIRTUAL_ADDRESS tlas) {
    std::lock_guard<std::mutex> g(g_lock);
    g_snapshots.erase(tlas);
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
    auto b = g_lastBuilt.find(tlas);
    pr.build = b != g_lastBuilt.end() ? b->second : 0;
    auto a = g_tlasAsSerial.find(tlas);
    pr.asOf = a != g_tlasAsSerial.end() ? a->second : 0;
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
            // An older build's read never replaces a newer one's: since
            // 0.52.0 every build is read, and one may have been parsed on
            // demand at a split (BringToBuild).
            auto r = g_readOf.find(pr.tlas);
            if (r == g_readOf.end() || r->second <= pr.build) {
                ParseLocked(pr.tlas,
                            static_cast<const D3D12_RAYTRACING_INSTANCE_DESC*>(p), pr.count,
                            pr.asOf);
                g_readOf[pr.tlas] = pr.build;
            }
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

UINT64 LatestBuild(D3D12_GPU_VIRTUAL_ADDRESS tlas) {
    std::lock_guard<std::mutex> g(g_lock);
    auto b = g_lastBuilt.find(tlas);
    return b == g_lastBuilt.end() ? 0 : b->second;
}

UINT64 SerialNow() {
    std::lock_guard<std::mutex> g(g_lock);
    return g_tlasSerial;
}

bool Current(const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& scenes) {
    std::lock_guard<std::mutex> g(g_lock);
    for (auto a : scenes)
        if (!CurrentLocked(a)) return false;
    return true;
}

bool LiveCurrent() {
    std::lock_guard<std::mutex> g(g_lock);
    for (const auto& kv : g_tlas)
        if (kv.second.valid && LiveLocked(kv.first) && !CurrentLocked(kv.first)) return false;
    return true;
}

bool BringToBuildLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas, UINT64 build, const void* list,
                        int depth);

bool BringToBuild(D3D12_GPU_VIRTUAL_ADDRESS tlas, UINT64 build, const void* list) {
    std::lock_guard<std::mutex> g(g_lock);
    return BringToBuildLocked(tlas, build, list, 0);
}

bool BringToBuildLocked(D3D12_GPU_VIRTUAL_ADDRESS tlas, UINT64 build, const void* list,
                        int depth) {
    auto r = g_readOf.find(tlas);
    if (r != g_readOf.end() && r->second == build) return true;
    // A copy of another structure: that one's build, then its answer.
    auto al = g_alias.find(tlas);
    if (al != g_alias.end() && al->second.build == build && depth < 8) {
        const Alias a = al->second;
        if (!BringToBuildLocked(a.src, a.srcBuild, list, depth + 1)) return false;
        auto st = g_tlas.find(a.src);
        if (st == g_tlas.end()) return false;
        g_tlas[tlas] = st->second;
        g_readOf[tlas] = build;
        return true;
    }
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
        PendingRead& pr = *it;
        if (pr.tlas != tlas || pr.build != build) continue;
        // Run already: recorded into the list being split, in a segment the
        // split has waited for; or stamped on a queue whose fence has passed.
        bool done = pr.owner == list;
        if (!done && pr.fenceValue) {
            auto f = g_fences.find(pr.queue);
            done = f != g_fences.end() && f->second.fence &&
                   f->second.fence->GetCompletedValue() >= pr.fenceValue;
        }
        if (!done) return false;
        const SIZE_T bytes = static_cast<SIZE_T>(pr.count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        void* p = nullptr;
        D3D12_RANGE readRange{ 0, bytes };
        bool ok = false;
        if (SUCCEEDED(pr.readback->Map(0, &readRange, &p)) && p) {
            ParseLocked(tlas, static_cast<const D3D12_RAYTRACING_INSTANCE_DESC*>(p), pr.count,
                        pr.asOf);
            g_readOf[tlas] = build;
            D3D12_RANGE noWrite{ 0, 0 };
            pr.readback->Unmap(0, &noWrite);
            ok = true;
        }
        g_pending.erase(it);
        return ok;
    }
    return false;
}

void NoteCopy(D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src,
              D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) {
    if (!dst) return;
    const bool same = mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE ||
                      mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT;
    // Serializing or decoding for tools writes a blob, not a structure.
    if (mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE ||
        mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_VISUALIZATION_DECODE_FOR_TOOLS)
        return;
    std::lock_guard<std::mutex> g(g_lock);
    auto hs = g_blasHist.find(src);
    const bool srcBlas = same && hs != g_blasHist.end() && !hs->second.empty();
    const bool srcTlas = same && g_lastBuilt.count(src) != 0;

    // A bottom-level structure: the source's geometry, or none known.
    BlasInfo bi;   // kind unknown, no geometries: refused when an instance points at it
    if (srcBlas) {
        bi = hs->second.back().second;
        // A copy of one of unknown geometry keeps that one's reason.
        if (bi.kind != Kind::kUnknown) bi.origin = 1;
    } else {
        bi.origin = mode == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE ? 2 : 3;
    }
    if (srcBlas || !srcTlas) {
        g_blas[dst] = bi;
        g_blasHist[dst].emplace_back(++g_asSerial, bi);
    }

    // A top-level structure: a new build of `dst` that IS the source's latest.
    if (srcTlas || g_lastBuilt.count(dst)) {
        g_lastBuilt[dst] = ++g_tlasSerial;
        g_firstBuilt.emplace(dst, g_tlasSerial);
        ++g_buildCount[dst];
        g_snapshots.erase(dst);
        g_alias.erase(dst);
        if (!srcTlas) {
            // Deserialized, or copied from something unknown: nothing known,
            // so a dispatch on it is refused as not read.
            g_tlas[dst].valid = false;
            g_readOf.erase(dst);
            return;
        }
        g_tlasAsSerial[dst] = g_tlasAsSerial[src];
        if (CurrentLocked(src)) {
            g_tlas[dst] = g_tlas[src];
            g_readOf[dst] = g_lastBuilt[dst];
            auto sn = g_snapshots.find(src);
            if (sn != g_snapshots.end()) g_snapshots[dst] = sn->second;
        } else {
            g_readOf.erase(dst);
            g_alias[dst] = Alias{ src, g_lastBuilt[src], g_lastBuilt[dst] };
        }
    }
}

bool UnknownBlas(const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& bound, bool exact) {
    std::lock_guard<std::mutex> g(g_lock);
    for (const auto& kv : g_tlas) {
        if (!kv.second.valid || !kv.second.unknownBlas) continue;
        if (exact ? std::find(bound.begin(), bound.end(), kv.first) != bound.end()
                  : LiveLocked(kv.first))
            return true;
    }
    return false;
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

bool AnyRead(const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& scenes) {
    std::lock_guard<std::mutex> g(g_lock);
    for (auto a : scenes) {
        auto it = g_tlas.find(a);
        if (it != g_tlas.end() && it->second.valid) return true;
    }
    return false;
}

bool TableWouldBeWrong(bool shaderCommitsProcedural, std::string* why,
                       const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* only) {
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
            if (!t.valid || !CountsLocked(kv.first, only)) continue;
            if (t.unknownBlas) {
                dstats::Add(dstats::kRefusedUnknownBlas);
                if (why)
                    *why = std::to_string(t.unknownBlas) + " instance(s) point at a bottom-level "
                           "structure whose geometry the shim does not know (deserialized, or "
                           "never seen built), so the records its hits reach cannot be known" +
                           t.unknownDetail;
                return true;
            }
            if (t.constantsConflict) {
                dstats::Add(dstats::kRefusedOneTlas);
                if (why)
                    *why = "two instances put different (contribution, geometry) "
                           "pairs on hit group record " +
                           std::to_string(t.conflictSlot) + t.conflictDetail +
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
        if (!t.valid || !CountsLocked(kv.first, only)) continue;
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

std::vector<RecordConstants> RecordConstantsTable(
    const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* only) {
    std::lock_guard<std::mutex> g(g_lock);
    std::vector<RecordConstants> out;
    for (const auto& kv : g_tlas) {
        const TlasInfo& t = kv.second;
        if (!t.valid || !CountsLocked(kv.first, only)) continue;
        if (out.size() < t.constants.size()) out.resize(t.constants.size());
        for (size_t i = 0; i < t.constants.size(); ++i)
            if (t.constants[i].assigned && !out[i].assigned) out[i] = t.constants[i];
    }
    return out;
}

std::vector<int32_t> GeometryLabels(const std::vector<std::pair<UINT, UINT>>& traceArgs,
                                    UINT records,
                                    const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& bound, bool exact,
                                    bool* read, bool* stale) {
    std::lock_guard<std::mutex> g(g_lock);
    std::vector<int32_t> out(records, -1);
    *read = false;
    *stale = false;
    bool anyBound = exact;
    for (auto a : bound) {
        auto it = g_tlas.find(a);
        if (it != g_tlas.end() && it->second.valid) anyBound = true;
    }
    for (const auto& kv : g_tlas) {
        const TlasInfo& t = kv.second;
        if (!t.valid) continue;
        if (anyBound ? std::find(bound.begin(), bound.end(), kv.first) == bound.end()
                     : !LiveLocked(kv.first))
            continue;
        *read = true;
        if (!CurrentLocked(kv.first)) *stale = true;
        for (const auto& c : t.classes)
            for (UINT gi = 0; gi < c.second; ++gi)
                for (const auto& rm : traceArgs) {
                    const UINT64 idx = (UINT64)(rm.first & 15) + (UINT64)(rm.second & 15) * gi +
                                       c.first;
                    if (idx >= records) continue;
                    int32_t& l = out[(size_t)idx];
                    if (l == -1) l = (int32_t)gi;
                    else if (l != (int32_t)gi) l = -2;
                }
    }
    return out;
}

std::vector<int> PairClasses(const std::vector<std::pair<UINT, UINT>>& pairs, UINT records,
                             const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& bound, bool exact) {
    std::lock_guard<std::mutex> g(g_lock);
    bool anyBound = exact;
    for (auto a : bound) {
        auto it = g_tlas.find(a);
        if (it != g_tlas.end() && it->second.valid) anyBound = true;
    }
    std::vector<std::pair<UINT, UINT>> classes;
    for (const auto& kv : g_tlas) {
        const TlasInfo& t = kv.second;
        if (!t.valid) continue;
        if (anyBound ? std::find(bound.begin(), bound.end(), kv.first) == bound.end()
                     : !LiveLocked(kv.first))
            continue;
        classes.insert(classes.end(), t.classes.begin(), t.classes.end());
    }
    std::map<std::vector<UINT>, int> ids;
    std::vector<int> out;
    for (const auto& rm : pairs) {
        std::vector<UINT> sig;
        bool some = false;
        for (const auto& c : classes)
            for (UINT gi = 0; gi < c.second; ++gi) {
                const UINT64 idx = (UINT64)(rm.first & 15) + (UINT64)(rm.second & 15) * gi + c.first;
                sig.push_back(idx < records ? (UINT)idx : UINT_MAX);
                some = some || idx < records;
            }
        if (!some) { out.push_back(-1); continue; }
        auto it = ids.find(sig);
        if (it == ids.end()) it = ids.emplace(sig, (int)ids.size()).first;
        out.push_back(it->second);
    }
    return out;
}

UINT MaxGeometryCount() {
    std::lock_guard<std::mutex> g(g_lock);
    UINT m = 1;
    for (const auto& kv : g_blas) m = (std::max)(m, kv.second.geometryCount);
    return m;
}

std::vector<uint8_t> RecordKinds(const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* only) {
    std::lock_guard<std::mutex> g(g_lock);
    std::vector<uint8_t> out;
    for (const auto& kv : g_tlas) {
        const TlasInfo& t = kv.second;
        if (!t.valid || !CountsLocked(kv.first, only)) continue;
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
