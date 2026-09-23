#include "rq_pipeline.h"

#include "config.h"

#include <dxgi1_4.h>
#include "proxy_log.h"
#include "rq_stub_cs.h"
#include "shader_dump.h"
#include "rewriter/dxc_host.h"
#include "rewriter/ll_model.h"
#include "rewriter/rq_analyze.h"
#include "rewriter/rq_lower.h"
#include "rewriter/rq_bake.h"

// Defined below, next to the rest of the crash instrumentation.
static void LogVideoMemory(ID3D12Device5* dev, const char* when);

#include "as_tracker.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <cstring>
#include <new>
#include <string>

const GUID IID_Dxr11RayQueryPso =
    { 0x7e3b1c42, 0x9a54, 0x4d18, { 0x8f, 0x60, 0x2c, 0x71, 0xb0, 0xa4, 0xe9, 0xd3 } };

namespace {

const UINT kIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;                  // 32
const UINT kRecAlign = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;        // 32
const UINT kTableAlign = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;       // 64
const UINT kPayloadBytes = 92;   // must match kPayloadBytes in rq_lower.cpp
const UINT kAttrBytes = 8;

UINT AlignUp(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }

// Context handed through the DXC host's transform callback.
struct Xform {
    std::string why;
    bool hasAnyHit = false;
    bool hasIntersection = false;
    bool needsBoth = false;
    // Does the shader ask what geometry or what instance contribution it hit?
    // Only then are the hit shaders baked, one copy per pair below.
    bool needsRecordConstants = false;
    int threads[3] = { 1, 1, 1 };
    // IN: the (geometryIndex, contribution) pairs to bake. At pipeline
    // creation the scene is unknown and this is the single pair (0, 0), which
    // is what the old local root signature table held before the first
    // dispatch too.
    std::vector<rq::RecordPair> pairs{ { 0u, 0u } };
};

// What the shim decided about this shader, in one line, so sotest can rebuild
// the same state object instead of inferring it from the bytes. `baked` is how
// many copies of the hit shaders the library carries, 0 when none.
std::string ShapeLine(const Xform& f) {
    char b[192];
    std::snprintf(b, sizeof(b),
                  "anyhit=%d intersection=%d both=%d recordconstants=%d baked=%u\n",
                  f.hasAnyHit ? 1 : 0, f.hasIntersection ? 1 : 0,
                  f.needsBoth ? 1 : 0, f.needsRecordConstants ? 1 : 0,
                  f.needsRecordConstants ? static_cast<unsigned>(f.pairs.size()) : 0u);
    return std::string(b);
}

bool DoLower(const std::string& in, std::string* out, std::string* why, void* ctx) {
    Xform* x = static_cast<Xform*>(ctx);
    llm::Module m(llm::Normalize(in));
    auto a = rq::Analyze(m);
    if (!a.ok) { *why = a.error; return false; }
    if (!rq::NumThreads(m, x->threads)) {
        *why = "the compute shader declares no numthreads, so the ray grid "
               "cannot be worked out";
        return false;
    }
    auto l = rq::Lower(m, a.query);
    if (!l.ok) { *why = l.error; return false; }
    x->hasAnyHit = a.query.NeedsAnyHit();
    x->hasIntersection = a.query.NeedsIntersection();
    x->needsBoth = a.query.NeedsBoth();
    x->needsRecordConstants = a.query.needsRecordConstants;
    *out = l.text;
    // Never hand the driver a hit shader that READS the record: bake it in.
    if (x->needsRecordConstants) {
        auto b = rq::Bake(l.text, x->pairs);
        if (!b.ok) { *why = b.error; return false; }
        *out = b.text;
    }
    return true;
}

// Everything the table needs from one state object.
struct Built {
    ID3D12StateObject* so = nullptr;
    uint8_t idRay[32]{}, idMiss[32]{}, idNullTri[32]{}, idNullProc[32]{};
    std::vector<std::array<uint8_t, 32>> idHit, idHitProc;
};

// Build the state object for a lowered (and, with record constants, baked)
// library, and read back every identifier the table needs.
//
// The application's compute root signature becomes the GLOBAL root signature,
// which is what makes its existing bindings work unchanged. There is NO local
// root signature: the record constants are baked into the hit shaders.
bool BuildStateObject(ID3D12Device5* dev, const std::vector<uint8_t>& lib,
                      const Xform& x, ID3D12RootSignature* globalRs,
                      Built* out, std::string* why) {
    const size_t copies = x.needsRecordConstants ? x.pairs.size() : 1;
    auto Name = [&](const wchar_t* base, size_t k) {
        std::wstring n = base;
        if (x.needsRecordConstants) n += L"_" + std::to_wstring(k);
        return n;
    };

    D3D12_DXIL_LIBRARY_DESC libDesc{};
    libDesc.DXILLibrary.pShaderBytecode = lib.data();
    libDesc.DXILLibrary.BytecodeLength = lib.size();

    // A hit group is one TYPE, taking an intersection shader where the other
    // takes an any-hit. A shader that commits both kinds therefore needs TWO,
    // each with its own closest-hit, because one reports committed status 1
    // and the other 2. With record constants there is one of each PER PAIR.
    //
    // The names live in vectors that are fully built before any descriptor
    // points into them, so nothing reallocates under a pointer.
    std::vector<std::wstring> names;
    names.reserve(copies * 6);
    std::vector<D3D12_HIT_GROUP_DESC> hgs(copies), hgProcs(x.needsBoth ? copies : 0);
    for (size_t k = 0; k < copies; ++k) {
        names.push_back(Name(L"HitGroup", k));
        names.push_back(Name(L"ClosestHit", k));
        names.push_back(Name(L"AnyHit", k));
        names.push_back(Name(L"Isect", k));
        names.push_back(Name(L"HitGroupProc", k));
        names.push_back(Name(L"ClosestHitProc", k));
    }
    for (size_t k = 0; k < copies; ++k) {
        const std::wstring* n = &names[k * 6];
        D3D12_HIT_GROUP_DESC& hg = hgs[k];
        hg.HitGroupExport = n[0].c_str();
        hg.ClosestHitShaderImport = n[1].c_str();
        if (x.needsBoth) {
            hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
            hg.AnyHitShaderImport = n[2].c_str();
            D3D12_HIT_GROUP_DESC& hp = hgProcs[k];
            hp.HitGroupExport = n[4].c_str();
            hp.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
            hp.IntersectionShaderImport = n[3].c_str();
            hp.ClosestHitShaderImport = n[5].c_str();
        } else if (x.hasIntersection) {
            hg.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
            hg.IntersectionShaderImport = n[3].c_str();
        } else {
            hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
            if (x.hasAnyHit) hg.AnyHitShaderImport = n[2].c_str();
        }
    }

    D3D12_RAYTRACING_SHADER_CONFIG sc{};
    sc.MaxPayloadSizeInBytes = kPayloadBytes;
    sc.MaxAttributeSizeInBytes = kAttrBytes;

    // Inline queries do not recurse, so one level is all a lowered shader can
    // ever need. The brief says as much.
    D3D12_RAYTRACING_PIPELINE_CONFIG pc{};
    pc.MaxTraceRecursionDepth = 1;

    D3D12_GLOBAL_ROOT_SIGNATURE grs{};
    grs.pGlobalRootSignature = globalRs;

    // Two more hit groups that never commit, one of each type. Which one a
    // scene needs is not knowable here, so both exist and the table picks.
    // A TRIANGLES group whose any-hit always ignores sees every triangle
    // candidate and rejects it; a PROCEDURAL group whose intersection shader
    // returns reports nothing. Neither carries a closest-hit, because neither
    // can ever reach one, and neither reads the record, so neither is copied.
    D3D12_HIT_GROUP_DESC hgNullTri{};
    hgNullTri.HitGroupExport = L"HitGroupNullTri";
    hgNullTri.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hgNullTri.AnyHitShaderImport = L"AnyHitNull";

    D3D12_HIT_GROUP_DESC hgNullProc{};
    hgNullProc.HitGroupExport = L"HitGroupNullProc";
    hgNullProc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
    hgNullProc.IntersectionShaderImport = L"IsectNull";

    std::vector<D3D12_STATE_SUBOBJECT> subs;
    subs.reserve(8 + hgs.size() + hgProcs.size());
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &libDesc });
    for (size_t k = 0; k < copies; ++k) {
        subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgs[k] });
        if (x.needsBoth)
            subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgProcs[k] });
    }
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgNullTri });
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgNullProc });
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });
    if (grs.pGlobalRootSignature)
        subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &grs });

    D3D12_STATE_OBJECT_DESC sod{};
    sod.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    sod.NumSubobjects = static_cast<UINT>(subs.size());
    sod.pSubobjects = subs.data();

    ID3D12StateObject* so = nullptr;
    // Logged BEFORE the call, and this is not a debug leftover. A generated
    // library once killed the device inside CreateStateObject, so that call
    // never returned and never logged a result. Without a line in front of it
    // the log ends on whatever came before and the call has to be inferred
    // from an absence. Now the last line in a dead log NAMES the call.
    LogVideoMemory(dev, "before CreateStateObject");
    ProxyLog("[dxr-tier-11-proxy-log] about to call CreateStateObject: %zu byte "
             "library, %u subobjects, %zu hit group cop%s, global root signature "
             "%p. If this is the last line in the log, that call is where the "
             "device went.\n",
             lib.size(), static_cast<unsigned>(subs.size()), copies,
             copies == 1 ? "y" : "ies", (void*)globalRs);
    HRESULT hr = dev->CreateStateObject(&sod, IID_PPV_ARGS(&so));
    if (FAILED(hr)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "CreateStateObject failed (hr=0x%08X)",
                      static_cast<unsigned>(hr));
        *why = buf;
        return false;
    }

    ID3D12StateObjectProperties* props = nullptr;
    if (FAILED(so->QueryInterface(IID_PPV_ARGS(&props)))) {
        so->Release();
        *why = "no ID3D12StateObjectProperties on the state object";
        return false;
    }

    const void* idRay = props->GetShaderIdentifier(L"RayGen");
    const void* idMiss = props->GetShaderIdentifier(L"Miss");
    const void* idNullTri = props->GetShaderIdentifier(L"HitGroupNullTri");
    const void* idNullProc = props->GetShaderIdentifier(L"HitGroupNullProc");
    bool ok = idRay && idMiss && idNullTri && idNullProc;
    for (size_t k = 0; ok && k < copies; ++k) {
        const void* h = props->GetShaderIdentifier(names[k * 6].c_str());
        if (!h) { ok = false; break; }
        std::array<uint8_t, 32> a{};
        std::memcpy(a.data(), h, kIdSize);
        out->idHit.push_back(a);
        if (x.needsBoth) {
            const void* p = props->GetShaderIdentifier(names[k * 6 + 4].c_str());
            if (!p) { ok = false; break; }
            std::memcpy(a.data(), p, kIdSize);
            out->idHitProc.push_back(a);
        }
    }
    if (!ok) {
        props->Release(); so->Release();
        out->idHit.clear(); out->idHitProc.clear();
        *why = "the state object does not export RayGen, Miss, every hit group "
               "and the two rejecting hit groups";
        return false;
    }
    std::memcpy(out->idRay, idRay, kIdSize);
    std::memcpy(out->idMiss, idMiss, kIdSize);
    std::memcpy(out->idNullTri, idNullTri, kIdSize);
    std::memcpy(out->idNullProc, idNullProc, kIdSize);
    props->Release();
    out->so = so;
    return true;
}

}  // namespace

Dxr11RayQueryPso::~Dxr11RayQueryPso() {
    // Before anything else: the carrier is releasing us, so nothing may find
    // us through it again.
    UnregisterCarrier(this);
    for (ID3D12Resource* r : m_retired) r->Release();
    for (ID3D12StateObject* s : m_retiredSo) s->Release();
    if (m_sbt) m_sbt->Release();
    if (m_so) m_so->Release();
    if (m_rootSig) m_rootSig->Release();
}

// carrier pipeline state -> the lowered query attached to it.
//
// GetPrivateData would answer this too, but it AddRefs on every call and
// SetPipelineState runs per draw. The carrier owns the query object, so the
// entry is removed from the query's own destructor and can never outlive it.
static std::mutex g_carrierLock;
static std::map<ID3D12PipelineState*, Dxr11RayQueryPso*> g_carriers;

void Dxr11RayQueryPso::RegisterCarrier(ID3D12PipelineState* carrier,
                                       Dxr11RayQueryPso* self) {
    std::lock_guard<std::mutex> g(g_carrierLock);
    g_carriers[carrier] = self;
}

void Dxr11RayQueryPso::UnregisterCarrier(Dxr11RayQueryPso* self) {
    std::lock_guard<std::mutex> g(g_carrierLock);
    for (auto it = g_carriers.begin(); it != g_carriers.end(); ++it)
        if (it->second == self) { g_carriers.erase(it); return; }
}

Dxr11RayQueryPso* Dxr11RayQueryPso::From(ID3D12PipelineState* p) {
    if (!p) return nullptr;
    std::lock_guard<std::mutex> g(g_carrierLock);
    auto it = g_carriers.find(p);
    return it == g_carriers.end() ? nullptr : it->second;
}

// The carrier, built once and used by every exit from TryCreate. `attach`
// false means a stopped phase: everything stays alive, but nothing registers,
// so no dispatch is ever substituted.
static ID3D12PipelineState* MakeCarrier(ID3D12Device5* dev,
                                        const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                        Dxr11RayQueryPso* self, bool attach,
                                        std::string* why) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
    cd.pRootSignature = desc->pRootSignature;
    cd.CS.pShaderBytecode = rqstub::kNullComputeDxil;
    cd.CS.BytecodeLength = rqstub::kNullComputeDxilSize;
    cd.NodeMask = desc->NodeMask;
    cd.Flags = desc->Flags;
    ID3D12PipelineState* carrier = nullptr;
    const HRESULT chr = dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&carrier));
    if (FAILED(chr) || !carrier) {
        char cbuf[96];
        std::snprintf(cbuf, sizeof(cbuf),
                      "cannot create the carrier pipeline state (hr=0x%08X)",
                      static_cast<unsigned>(chr));
        if (why) *why = cbuf;
        if (self) self->Release();
        return nullptr;
    }
    if (self) {
        if (attach) Dxr11RayQueryPso::RegisterCarrier(carrier, self);
        carrier->SetPrivateDataInterface(IID_Dxr11RayQueryPso, self);
        self->Release();
    }
    return carrier;
}
// rqphase: stop part-way through and hand back a do-nothing pipeline.
//
// rqstub = 1 runs the game and full lowering crashes the driver, so the cause
// is somewhere between those two points. Everything in between has now been
// replayed offline and none of it reproduces: the libraries build one at a
// time, 210 at once, and on eight threads at once, with the device alive
// afterwards. DRED says the GPU was executing nothing when it died.
//
// So bisect the middle instead of guessing at it. Each phase does one more
// step than the last and then stops:
//
//   rqphase = 0   nothing (same as rqstub)
//   rqphase = 1   rewrite the DXIL, throw it away
//   rqphase = 2   rewrite, then create the state object
//   rqphase = 3   rewrite, state object, then build the shader table
//   absent        all of it, the normal behaviour
//
// A stopped phase still keeps everything it built alive, attached to the
// carrier, so memory and driver objects match the real run. What it does not
// do is register the carrier, so no dispatch is ever substituted and the
// pipeline does nothing. Rendering will be wrong, exactly as under rqstub.
int Dxr11RayQueryPhase();

static int RayQueryPhase() {
    static int s = -2;
    if (s == -2) {
        const cfg::Text t = cfg::GetText("DXR_TIER11_RQPHASE", "rqphase");
        s = t.value.empty() ? -1 : _wtoi(t.value.c_str());
        if (s >= 0)
            ProxyLog("[dxr-tier-11-proxy-log] rqphase = %d (from %s): lowering stops "
                     "after that step and the application gets a pipeline that does "
                     "nothing. This is a bisect, not a setting.\n", s, t.source);
    }
    return s;
}
int Dxr11RayQueryPhase() { return RayQueryPhase(); }

void Dxr11RayQueryPhaseNote(const char* what, const char* detail) {
    if (RayQueryPhase() < 0) return;
    static LONG s_n = 0;
    const LONG i = InterlockedIncrement(&s_n);
    ProxyLog("[dxr-tier-11-proxy-log] rqphase: RayQuery shader %ld %s%s%s\n",
             static_cast<long>(i), what,
             (detail && *detail) ? ": " : "", (detail && *detail) ? detail : "");
}

// What the GPU's memory looks like at the moment of a state object build.
//
// One generated library kills the device inside CreateStateObject in a
// shipping game and builds perfectly on the same card offline: alone, sixty
// times over, and with 261 other state objects already held. Everything about
// the inputs is therefore exonerated and the difference is what else the
// device has been asked to do. Memory is the first thing about a loaded game
// that an empty probe cannot reproduce, and a driver that cannot allocate for
// a shader it is compiling is a plausible source of
// DXGI_ERROR_DRIVER_INTERNAL_ERROR, which carries no detail of its own.
//
// This measures rather than argues. If usage is nowhere near budget the idea
// is dead in one run, which is worth more than the idea being right.
//
// dxgi.dll is loaded by NAME on purpose here, unlike DXC. The point is to
// reach whatever DXGI this process is already using, and if that is this
// project's own dxgi proxy then it forwards and the answer is the same.
static void LogVideoMemory(ID3D12Device5* dev, const char* when) {
    if (!dev) return;
    IDXGIAdapter3* ad = nullptr;
    HMODULE m = LoadLibraryW(L"dxgi.dll");
    if (m) {
        typedef HRESULT(WINAPI * PFN_CF1)(REFIID, void**);
        auto cf1 = reinterpret_cast<PFN_CF1>(
            reinterpret_cast<void*>(GetProcAddress(m, "CreateDXGIFactory1")));
        IDXGIFactory4* f = nullptr;
        if (cf1 && SUCCEEDED(cf1(__uuidof(IDXGIFactory4), (void**)&f)) && f) {
            f->EnumAdapterByLuid(dev->GetAdapterLuid(),
                                 __uuidof(IDXGIAdapter3), (void**)&ad);
            f->Release();
        }
    }
    if (!ad) {
        ProxyLog("[dxr-tier-11-proxy-log] video memory %s: could not reach "
                 "IDXGIAdapter3, so this run has no memory figure\n", when);
        return;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO loc{}, non{};
    const HRESULT h1 = ad->QueryVideoMemoryInfo(
        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &loc);
    const HRESULT h2 = ad->QueryVideoMemoryInfo(
        0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non);
    ad->Release();
    const double MB = 1024.0 * 1024.0;
    if (SUCCEEDED(h1))
        ProxyLog("[dxr-tier-11-proxy-log] video memory %s: LOCAL usage %.0f MB "
                 "of budget %.0f MB (%.0f%%), reserved %.0f MB\n",
                 when, loc.CurrentUsage / MB, loc.Budget / MB,
                 loc.Budget ? (100.0 * loc.CurrentUsage / loc.Budget) : 0.0,
                 loc.CurrentReservation / MB);
    if (SUCCEEDED(h2))
        ProxyLog("[dxr-tier-11-proxy-log] video memory %s: SYSTEM usage %.0f MB "
                 "of budget %.0f MB\n",
                 when, non.CurrentUsage / MB, non.Budget / MB);
}

// rqlimit and rqonly: cap or single out which shaders actually get BUILT.
//
// Both count only shaders that reached this point, meaning they LOWERED.
// Counting the ones the rewriter refused made "rqlimit = 1" spend its budget
// on a shader that never built anything, and three runs answered nothing.
//
//   rqlimit = N   build the first N that lower
//   rqonly  = N   build ONLY the N-th (0-based), refuse every other
//
// rqonly exists because rqlimit can only ask "how many", and the bisect it
// produced ended on a question it cannot answer: the seventh shader kills
// the device in the game and builds perfectly offline, so the next thing to
// separate is whether that shader alone does it or whether it needs the
// other six present. rqonly takes precedence when both are set.
static bool RayQueryLimitReached(std::string* why) {
    static int s_limit = -2;
    static int s_only = -2;
    static LONG s_built = 0;
    if (s_limit == -2) {
        const cfg::Text t = cfg::GetText("DXR_TIER11_RQLIMIT", "rqlimit");
        s_limit = t.value.empty() ? -1 : _wtoi(t.value.c_str());
        if (s_limit >= 0)
            ProxyLog("[dxr-tier-11-proxy-log] rqlimit = %d (from %s): at most %d "
                     "RayQuery shaders will have a state object built for them. "
                     "Shaders the rewriter refuses do not count against it. This is "
                     "a BISECT for a driver crash, not something to leave set.\n",
                     s_limit, t.source, s_limit);
    }
    if (s_only == -2) {
        const cfg::Text t = cfg::GetText("DXR_TIER11_RQONLY", "rqonly");
        s_only = t.value.empty() ? -1 : _wtoi(t.value.c_str());
        if (s_only >= 0)
            ProxyLog("[dxr-tier-11-proxy-log] rqonly = %d (from %s): ONLY the %d-th "
                     "RayQuery shader to lower gets a state object, counting from 0. "
                     "Every other one is refused even though it lowered. This "
                     "overrides rqlimit, and is a BISECT, not something to leave "
                     "set.\n",
                     s_only, t.source, s_only);
    }
    if (s_only < 0 && s_limit < 0) return false;

    const LONG n = InterlockedIncrement(&s_built) - 1;
    char buf[224];
    if (s_only >= 0) {
        if (n == s_only) return false;
        std::snprintf(buf, sizeof(buf),
                      "rqonly = %d; this shader DID lower but is the %ld-th to do "
                      "so, and is being forwarded on purpose",
                      s_only, static_cast<long>(n));
        *why = buf;
        return true;
    }
    if (n < s_limit) return false;
    std::snprintf(buf, sizeof(buf),
                  "rqlimit = %d reached; this shader DID lower but is the %ld-th to "
                  "do so, and is being forwarded on purpose",
                  s_limit, static_cast<long>(n));
    *why = buf;
    return true;
}

ID3D12PipelineState* Dxr11RayQueryPso::TryCreate(
        ID3D12Device5* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
        std::string* why) {
    if (!dev || !desc || !desc->CS.pShaderBytecode) {
        *why = "no compute shader bytecode";
        return nullptr;
    }

    // --- rewrite ------------------------------------------------------------
    Xform x;
    std::vector<uint8_t> lib;
    std::string err;
    if (!dxch::RewriteContainer(desc->CS.pShaderBytecode,
                                static_cast<size_t>(desc->CS.BytecodeLength),
                                DoLower, &x, &lib, &err)) {
        *why = err;
        return nullptr;
    }

    if (RayQueryPhase() == 1) {
        Dxr11RayQueryPhaseNote("rewritten, nothing built", nullptr);
        return MakeCarrier(dev, desc, nullptr, false, why);
    }

    // A bisect, not a setting. Here, after the rewrite and before anything is
    // built, so the budget is spent only on shaders that actually lower.
    if (RayQueryLimitReached(why)) return nullptr;

    // Before CreateStateObject, deliberately. A library that lowers cleanly
    // and then kills the driver is exactly the case worth having on disk, and
    // dumping after the call would miss it.
    shdump::Lowered(desc->CS.pShaderBytecode,
                    static_cast<size_t>(desc->CS.BytecodeLength),
                    lib.data(), lib.size(), desc->pRootSignature,
                    ShapeLine(x).c_str());

    // --- state object -------------------------------------------------------
    Built b;
    if (!BuildStateObject(dev, lib, x, desc->pRootSignature, &b, why))
        return nullptr;

    auto* self = new (std::nothrow) Dxr11RayQueryPso();
    if (!self) {
        b.so->Release();
        *why = "out of memory";
        return nullptr;
    }
    self->m_dev = dev;
    self->m_so = b.so;
    self->m_rootSig = desc->pRootSignature;
    if (self->m_rootSig) self->m_rootSig->AddRef();
    for (int i = 0; i < 3; ++i) self->m_threads[i] = static_cast<UINT>(x.threads[i]);
    // A both-kinds shader serves both; otherwise exactly one.
    self->m_servesProc = x.hasIntersection;
    self->m_servesTri = x.needsBoth || !x.hasIntersection;
    self->m_hasAnyHit = x.hasAnyHit;
    self->m_hasIntersection = x.hasIntersection;
    self->m_needsBoth = x.needsBoth;
    self->m_recordConstants = x.needsRecordConstants;
    if (x.needsRecordConstants) {
        // A pair the scene needs and this state object lacks means lowering
        // again at dispatch, from the shader as the application gave it.
        const auto* p = static_cast<const uint8_t*>(desc->CS.pShaderBytecode);
        self->m_input.assign(p, p + desc->CS.BytecodeLength);
        self->m_pairs = x.pairs;
    }
    std::memcpy(self->m_idRay, b.idRay, kIdSize);
    std::memcpy(self->m_idMiss, b.idMiss, kIdSize);
    std::memcpy(self->m_idNullTri, b.idNullTri, kIdSize);
    std::memcpy(self->m_idNullProc, b.idNullProc, kIdSize);
    self->m_idHit = b.idHit;
    self->m_idHitProc = b.idHitProc;

    if (RayQueryPhase() == 2) {
        Dxr11RayQueryPhaseNote("lowered, state object BUILT", nullptr);
        return MakeCarrier(dev, desc, self, false, why);
    }

    // One record of the real hit group to begin with. The acceleration
    // structures have usually not been built yet at pipeline creation, so what
    // the scene needs is not knowable here; the first dispatch rebuilds the
    // table once it is.
    if (!self->BuildTable(std::vector<uint8_t>(),
                          std::vector<rq::RecordPair>(), why)) {
        self->Release();
        return nullptr;
    }

    if (RayQueryPhase() == 3) {
        Dxr11RayQueryPhaseNote("lowered, state object and shader table BUILT", nullptr);
        return MakeCarrier(dev, desc, self, false, why);
    }

    static LONG onceVer = 0;
    if (InterlockedCompareExchange(&onceVer, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] rewriter using %s\n", dxch::Versions());

    {
        static LONG s_next = 0;
        self->m_index = static_cast<int>(InterlockedIncrement(&s_next) - 1);
    }

    ProxyLog("[dxr-tier-11-proxy-log] RayQuery compute shader lowered and ready: "
             "%zu -> %zu bytes, numthreads(%u,%u,%u)%s\n",
             static_cast<size_t>(desc->CS.BytecodeLength), lib.size(),
             self->m_threads[0], self->m_threads[1], self->m_threads[2],
             x.needsBoth ? ", with a generated any-hit AND intersection shader"
                 : x.hasIntersection ? ", with a generated intersection shader"
                 : (x.hasAnyHit ? ", with a generated any-hit shader" : ""));
    // The object the application holds is a REAL pipeline state: its own root
    // signature, and a compute shader that does nothing. It is never executed,
    // because Dispatch is intercepted and replaced by SetPipelineState1 plus
    // DispatchRays. What matters is that D3D12 made it, so every call the
    // application makes on it is handled by D3D12 rather than by guesswork.
    return MakeCarrier(dev, desc, self, true, why);
}

bool Dxr11RayQueryPso::BuildTable(const std::vector<uint8_t>& kinds,
                                  const std::vector<rq::RecordPair>& recPairs,
                                  std::string* why) {
    const UINT hitRecords =
        kinds.empty() ? 1u : static_cast<UINT>(kinds.size());

    // Which baked hit group copy each record points at. Every pair a record
    // needs must already be in m_pairs; the caller rebakes first.
    std::vector<size_t> copyOf(hitRecords, 0);
    if (m_recordConstants) {
        for (UINT i = 0; i < hitRecords; ++i) {
            const rq::RecordPair p = i < recPairs.size() ? recPairs[i] : rq::RecordPair{};
            const auto it = std::find(m_pairs.begin(), m_pairs.end(), p);
            if (it == m_pairs.end()) {
                if (why) *why = "a record needs a (geometry, contribution) pair the "
                                "state object was not baked with";
                return false;
            }
            copyOf[i] = static_cast<size_t>(it - m_pairs.begin());
        }
    }

    // raygen, then miss, then the hit group records, each TABLE aligned to 64
    // and each RECORD to 32.
    //
    // A record is the identifier and nothing else. What it tells the hit
    // shader, its geometry index and instance contribution, is baked into the
    // hit group it points at, because READING a local root signature from a
    // hit shader crashes the Pascal driver. See proxy/rewriter/rq_bake.h.
    const UINT stride = AlignUp(kIdSize, kRecAlign);
    const UINT slot = AlignUp(stride, kTableAlign);
    const UINT hitBytes = AlignUp(stride * hitRecords, kTableAlign);
    const UINT64 size = static_cast<UINT64>(slot) * 2 + hitBytes;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* sbt = nullptr;
    if (FAILED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&sbt)))) {
        if (why) *why = "cannot create the shader table buffer";
        return false;
    }

    uint8_t* p = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(sbt->Map(0, &none, reinterpret_cast<void**>(&p)))) {
        sbt->Release();
        if (why) *why = "cannot map the shader table";
        return false;
    }
    std::memset(p, 0, static_cast<size_t>(size));
    std::memcpy(p + 0 * slot, m_idRay, kIdSize);
    std::memcpy(p + 1 * slot, m_idMiss, kIdSize);

    // One record per index, of the TYPE the geometry reaching that index needs.
    // The application's contributions decide which slot a hit lands on; this
    // decides what is in the slot.
    for (UINT i = 0; i < hitRecords; ++i) {
        const uint8_t reach = i < kinds.size() ? kinds[i] : astrack::kReachNone;
        const bool tri = (reach & astrack::kReachTriangles) != 0;
        const bool proc = (reach & astrack::kReachProcedural) != 0;

        // Default to the shader's own hit group, which is right for a slot
        // reached by the kind it serves, by both when it serves only one, or
        // by nothing at all. A slot reached by BOTH when the shader serves
        // both is unservable and was refused before this call.
        //
        // "The shader's own hit group" is the copy baked with THIS record's
        // pair: that is how the record tells the shader what it hit. Nothing
        // in DXR 1.0 can ask, which is the whole reason these accessors were
        // refused before the shim owned the table.
        const size_t k = copyOf[i];
        const void* own = m_idHit[k].data();
        const void* id = own;
        if (proc && !tri) {
            // Procedural only: the real procedural group if there is one, and
            // otherwise a procedural record that reports nothing, so the
            // geometry is traversed with a record of the right TYPE.
            id = m_servesProc ? (m_servesTri ? m_idHitProc[k].data() : own)
                              : m_idNullProc;
        } else if (tri && !proc) {
            // Triangles only. m_idHit is already the triangle group whenever
            // the shader serves triangles at all.
            id = m_servesTri ? own : m_idNullTri;
        }
        uint8_t* rec = p + 2 * slot + i * stride;
        std::memcpy(rec, id, kIdSize);
    }
    sbt->Unmap(0, nullptr);

    if (m_sbt) m_retired.push_back(m_sbt);   // may still be in flight
    m_sbt = sbt;
    m_kinds = kinds;
    m_recPairs = recPairs;

    const D3D12_GPU_VIRTUAL_ADDRESS base = sbt->GetGPUVirtualAddress();
    m_desc.RayGenerationShaderRecord.StartAddress = base + 0 * slot;
    m_desc.RayGenerationShaderRecord.SizeInBytes = kIdSize;
    m_desc.MissShaderTable.StartAddress = base + 1 * slot;
    m_desc.MissShaderTable.SizeInBytes = kIdSize;
    m_desc.MissShaderTable.StrideInBytes = stride;
    m_desc.HitGroupTable.StartAddress = base + 2 * slot;
    m_desc.HitGroupTable.SizeInBytes = stride * hitRecords;
    m_desc.HitGroupTable.StrideInBytes = stride;
    return true;
}

void Dxr11RayQueryPso::DispatchAsRays(ID3D12GraphicsCommandList4* cl,
                                      UINT gx, UINT gy, UINT gz,
                                      const std::vector<uint8_t>& recordKinds) {
    if (!cl) return;

    // Command lists are recorded on several threads and this can rebuild both
    // the table and the state object, so the whole decision is made under one
    // lock and what is recorded afterwards is a snapshot of its outcome.
    ID3D12StateObject* so = nullptr;
    D3D12_DISPATCH_RAYS_DESC d{};
    {
    std::lock_guard<std::mutex> guard(m_lock);

    // Read once here rather than inside BuildTable, so the table and the kinds
    // it was built from come from the same moment. One pair per record: the
    // geometry index and instance contribution a hit on that record reports.
    std::vector<rq::RecordPair> recPairs;
    if (m_recordConstants && !recordKinds.empty()) {
        const std::vector<astrack::RecordConstants> consts = astrack::RecordConstantsTable();
        recPairs.resize(recordKinds.size());
        for (size_t i = 0; i < recPairs.size() && i < consts.size(); ++i)
            recPairs[i] = { consts[i].geometryIndex, consts[i].instanceContribution };
    }

    // A record whose pair has no baked hit group means lowering again. The set
    // only grows, like the table, so a scene needing fewer keeps what it has
    // and a scene that flips back and forth does not rebake every time.
    if (m_recordConstants) {
        std::vector<rq::RecordPair> grown = m_pairs;
        for (const auto& p : recPairs)
            if (std::find(grown.begin(), grown.end(), p) == grown.end()) grown.push_back(p);
        if (grown.size() != m_pairs.size()) {
            const size_t had = m_pairs.size();
            const DWORD t0 = GetTickCount();
            std::string why;
            if (!Rebake(grown, &why)) {
                ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch SKIPPED: the "
                         "scene needs %zu (geometry, contribution) pairs and the hit "
                         "shaders could not be rebaked for them (%s).\n",
                         grown.size(), why.c_str());
                return;
            }
            ProxyLog("[dxr-tier-11-proxy-log] hit shaders rebaked for the scene: %zu -> "
                     "%zu (geometry, contribution) pairs, %lu ms, one hit group per "
                     "pair and no local root signature.\n",
                     had, grown.size(), static_cast<unsigned long>(GetTickCount() - t0));
            m_kinds.clear();   // the identifiers moved, so the table must follow
        }
    }

    // Rebuild when the scene's layout is not what the table was built for,
    // which is the normal case the first time: the acceleration structures did
    // not exist when the pipeline was created. A changed layout can mean more
    // records OR the same number with different types in them, or, with
    // record constants, records carrying different numbers.
    if (!recordKinds.empty() && (recordKinds != m_kinds || recPairs != m_recPairs)) {
        const size_t had = m_kinds.size();
        std::string why;
        if (!BuildTable(recordKinds, recPairs, &why)) {
            ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch SKIPPED: the scene "
                     "needs %u hit group records and the table could not be "
                     "rebuilt (%s).\n",
                     static_cast<unsigned>(recordKinds.size()), why.c_str());
            return;
        }
        UINT rejecting = 0;
        for (uint8_t k : recordKinds) {
            const bool tri = (k & astrack::kReachTriangles) != 0;
            const bool proc = (k & astrack::kReachProcedural) != 0;
            if (proc && !tri && !m_servesProc) ++rejecting;
            else if (tri && !proc && !m_servesTri) ++rejecting;
        }
        ProxyLog("[dxr-tier-11-proxy-log] shader table rebuilt for the scene: %u -> %u hit "
                 "group records, %u of them rejecting geometry this shader does "
                 "not serve (it commits %s).\n",
                 static_cast<unsigned>(had),
                 static_cast<unsigned>(recordKinds.size()), rejecting,
                 (m_servesTri && m_servesProc) ? "both kinds"
                     : m_servesProc ? "procedural hits" : "triangle hits");
    }

    // The state object and the table this dispatch uses, as one consistent
    // pair. A later rebake retires both rather than releasing them.
    so = m_so;
    d = m_desc;
    }   // m_lock

    // rqdispatch: build everything, execute only the first N pipelines.
    //
    // This is the bisect that rqlimit should have been. rqlimit forwards the
    // shaders it skips, and THIS PROJECT ALREADY KNEW that Unreal turns a
    // forwarded RayQuery shader into `Shader compilation failures are Fatal`.
    // So rqlimit = 0 killed the game at the first shader, long before the
    // point where the driver had been dying, and proved almost nothing.
    //
    // Skipping the DISPATCH instead keeps every state object built and every
    // pipeline handed over, so the application is happy and runs. What changes
    // is only whether the GPU is asked to execute the lowered work. That
    // separates "the driver cannot compile our libraries" from "the driver
    // cannot execute our rays", which is the split that matters and the one
    // nothing so far has tested.
    {
        static int s_limit = -2;
        if (s_limit == -2) {
            const cfg::Text t = cfg::GetText("DXR_TIER11_RQDISPATCH", "rqdispatch");
            s_limit = t.value.empty() ? -1 : _wtoi(t.value.c_str());
            if (s_limit >= 0)
                ProxyLog("[dxr-tier-11-proxy-log] rqdispatch = %d (from %s): lowered "
                         "pipelines are still built, but only the first %d will actually "
                         "dispatch. Rendering will be WRONG. This is a bisect for a driver "
                         "crash, not something to leave set.\n", s_limit, t.source, s_limit);
        }
        if (s_limit >= 0 && m_index >= s_limit) {
            static LONG s_said = 0;
            if (InterlockedCompareExchange(&s_said, 1, 0) == 0)
                ProxyLog("[dxr-tier-11-proxy-log] rqdispatch: pipeline %d and beyond are "
                         "built but NOT dispatched\n", m_index);
            return;
        }
    }

    // Dispatch counts thread GROUPS; DispatchRays counts RAYS. The lowered
    // raygen reads DispatchRaysIndex where the original read
    // SV_DispatchThreadID, which is the global thread id, so the ray grid is
    // the group count times the group size. The shader's own bounds check
    // discards the overhang, exactly as it did for threads.
    d.Width = gx * m_threads[0];
    d.Height = gy * m_threads[1];
    d.Depth = gz * m_threads[2];
    cl->SetPipelineState1(so);
    cl->DispatchRays(&d);
}

bool Dxr11RayQueryPso::Rebake(const std::vector<rq::RecordPair>& pairs,
                              std::string* why) {
    if (m_input.empty()) {
        *why = "the original shader was not kept, so it cannot be lowered again";
        return false;
    }
    Xform x;
    x.pairs = pairs;
    std::vector<uint8_t> lib;
    std::string err;
    if (!dxch::RewriteContainer(m_input.data(), m_input.size(), DoLower, &x, &lib, &err)) {
        *why = err;
        return false;
    }
    // Dumped like the first build, and for the same reason: a library that
    // lowers and then kills the driver is the one worth having on disk.
    shdump::Lowered(m_input.data(), m_input.size(), lib.data(), lib.size(),
                    m_rootSig, ShapeLine(x).c_str());
    Built b;
    if (!BuildStateObject(m_dev, lib, x, m_rootSig, &b, why)) return false;

    if (m_so) m_retiredSo.push_back(m_so);   // may still be in flight
    m_so = b.so;
    m_pairs = pairs;
    std::memcpy(m_idRay, b.idRay, kIdSize);
    std::memcpy(m_idMiss, b.idMiss, kIdSize);
    std::memcpy(m_idNullTri, b.idNullTri, kIdSize);
    std::memcpy(m_idNullProc, b.idNullProc, kIdSize);
    m_idHit = b.idHit;
    m_idHitProc = b.idHitProc;
    return true;
}

// --- IUnknown ---------------------------------------------------------------

HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::QueryInterface(REFIID riid, void** pp) {
    if (!pp) return E_POINTER;
    if (riid == IID_Dxr11RayQueryPso) {
        // Deliberately no AddRef: this is recognition, not ownership.
        *pp = this;
        return S_OK;
    }
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12PipelineState)) {
        AddRef();
        *pp = static_cast<ID3D12PipelineState*>(this);
        return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dxr11RayQueryPso::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&m_refs));
}

ULONG STDMETHODCALLTYPE Dxr11RayQueryPso::Release() {
    const LONG n = InterlockedDecrement(&m_refs);
    if (n == 0) delete this;
    return static_cast<ULONG>(n);
}

// --- the rest is bookkeeping the application may call but nothing depends on -

HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::GetPrivateData(REFGUID g, UINT* s, void* d) {
    return m_so ? m_so->GetPrivateData(g, s, d) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::SetPrivateData(REFGUID g, UINT s, const void* d) {
    return m_so ? m_so->SetPrivateData(g, s, d) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::SetPrivateDataInterface(REFGUID g, const IUnknown* u) {
    return m_so ? m_so->SetPrivateDataInterface(g, u) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::SetName(LPCWSTR n) {
    return m_so ? m_so->SetName(n) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::GetDevice(REFIID riid, void** pp) {
    return m_dev ? m_dev->QueryInterface(riid, pp) : E_FAIL;
}
HRESULT STDMETHODCALLTYPE Dxr11RayQueryPso::GetCachedBlob(ID3DBlob** pp) {
    // A lowered shader has no cached blob that would mean anything to the
    // application, and handing back the real one would be a lie.
    if (pp) *pp = nullptr;
    return E_NOTIMPL;
}
