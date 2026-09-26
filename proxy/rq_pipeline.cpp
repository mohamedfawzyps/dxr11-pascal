#include "rq_pipeline.h"

#include "dispatch_stats.h"
#include "gpu_hold.h"

#include "config.h"

#include <dxgi1_4.h>
#include "proxy_log.h"
#include "rq_stub_cs.h"
#include "shader_dump.h"
#include "rewriter/dxc_host.h"
#include "rewriter/geom_index.h"
#include "rewriter/ll_model.h"
#include "rewriter/nvapi_fold.h"
#include "rewriter/rq_analyze.h"
#include "rewriter/rq_lower.h"

// Defined below, next to the rest of the crash instrumentation.
static void LogVideoMemory(ID3D12Device5* dev, const char* when);

#include "as_tracker.h"
#include "shim_scene.h"

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
    // Only then do the hit records carry a pointer to their pair.
    bool needsRecordConstants = false;
    // The loop body appends to a buffer; see rq::LowerResult::appends.
    bool appends = false;
    UINT payloadBytes = kPayloadBytes;   // grows with values the payload carries
    int carried = 0;
    int threads[3] = { 1, 1, 1 };
    gidx::Scenes scenes;   // where the lowered TraceRay calls take their scene
};

// What the shim decided about this shader, in one line, so sotest can rebuild
// the same state object instead of inferring it from the bytes. `baked=0` and
// `recordsrv=1`: no hit shader copies, and the record is read through a local
// root SRV, which is the local root signature sotest has to build.
std::string ShapeLine(const Xform& f) {
    char b[192];
    std::snprintf(b, sizeof(b),
                  "anyhit=%d intersection=%d both=%d recordconstants=%d baked=0 recordsrv=%d"
                  " payload=%u\n",
                  f.hasAnyHit ? 1 : 0, f.hasIntersection ? 1 : 0,
                  f.needsBoth ? 1 : 0, f.needsRecordConstants ? 1 : 0,
                  f.needsRecordConstants ? 1 : 0, f.payloadBytes);
    return std::string(b);
}

bool DoLower(const std::string& in, std::string* out, std::string* why, void* ctx) {
    Xform* x = static_cast<Xform*>(ctx);
    std::string text;
    if (!rq::FoldNvapi(llm::Normalize(in), &text, why)) return false;
    llm::Module m(text);
    auto a = rq::Analyze(m);
    if (!a.ok) { *why = a.error; return false; }
    if (!rq::NumThreads(m, x->threads)) {
        *why = "the compute shader declares no numthreads, so the ray grid "
               "cannot be worked out";
        return false;
    }
    auto l = rq::Lower(m, a.query);
    if (!l.ok) { *why = l.error; return false; }
    x->hasAnyHit = a.query.AnyNeedsAnyHit();
    x->hasIntersection = a.query.NeedsIntersection();
    x->needsBoth = a.query.NeedsBoth();
    x->needsRecordConstants = a.query.needsRecordConstants;
    x->appends = l.appends;
    x->payloadBytes = (UINT)l.payloadBytes;
    x->carried = l.carried;
    gidx::ScanScenes(l.text, &x->scenes);
    *out = l.text;
    return true;
}

// Everything the table needs from one state object.
struct Built {
    ID3D12StateObject* so = nullptr;
    ID3D12RootSignature* localRs = nullptr;   // owned by the caller once built
    ID3D12RootSignature* raygenRs = nullptr;  // likewise; only with raygenSrvs
    uint8_t idRay[32]{}, idMiss[32]{}, idNullTri[32]{}, idNullProc[32]{};
    std::vector<std::array<uint8_t, 32>> idHit, idHitProc;
};

// Build the state object for a lowered library, and read back every
// identifier the table needs.
//
// The application's compute root signature becomes the GLOBAL root signature,
// which is what makes its existing bindings work unchanged. With record data,
// a LOCAL root signature holding one root SRV at t0, space1 gives each hit
// record a pointer to its own (geometry, contribution) pair. A root SRV and
// NOT root constants or a root CBV: a hit shader reading a CBUFFER through the
// local root signature crashes the Pascal driver, and the same library reading
// through a local root SRV was 0 of 30 cold compiles. See
// phase5/cases/driver-crash/README.md.
// `raygenSrvs` (0.61.0, the shim's own layout): the raygen gets a local root
// signature of that many root SRVs, t0 upwards in space rq::kGeomIndexSpace,
// where the retraced calls find the shim's scene copies and key table.
bool BuildStateObject(ID3D12Device5* dev, const std::vector<uint8_t>& lib,
                      const Xform& x, ID3D12RootSignature* globalRs,
                      Built* out, std::string* why, UINT raygenSrvs = 0) {
    const size_t copies = 1;
    auto Name = [&](const wchar_t* base, size_t) { return std::wstring(base); };

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
    sc.MaxPayloadSizeInBytes = x.payloadBytes;
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

    // Associated with the HIT GROUPS only, never made the default. A default
    // local root signature would apply to the raygen and miss records too, and
    // those would then need room for an argument they never read. The two
    // rejecting groups get it as well, so every hit record has one layout.
    ID3D12RootSignature* localRs = nullptr;
    D3D12_LOCAL_ROOT_SIGNATURE lrs{};
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc{};
    const wchar_t* hitExports[4] = {};
    UINT hitExportCount = 0;
    if (x.needsRecordConstants) {
        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp.Descriptor.ShaderRegister = 0;
        rp.Descriptor.RegisterSpace = 1;
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters = &rp;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT rhr = D3D12SerializeRootSignature(
            &rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
        if (SUCCEEDED(rhr))
            rhr = dev->CreateRootSignature(0, blob->GetBufferPointer(),
                                           blob->GetBufferSize(),
                                           IID_PPV_ARGS(&localRs));
        if (blob) blob->Release();
        if (err) err->Release();
        if (FAILED(rhr) || !localRs) {
            *why = "cannot create the local root signature that carries the "
                   "record's (geometry, contribution) pair";
            return false;
        }
        lrs.pLocalRootSignature = localRs;
        hitExports[hitExportCount++] = names[0].c_str();          // HitGroup
        if (x.needsBoth) hitExports[hitExportCount++] = names[4].c_str();
        hitExports[hitExportCount++] = L"HitGroupNullTri";
        hitExports[hitExportCount++] = L"HitGroupNullProc";
        assoc.NumExports = hitExportCount;
        assoc.pExports = hitExports;
    }

    // The raygen's, in the shim's own layout.
    ID3D12RootSignature* raygenRs = nullptr;
    D3D12_LOCAL_ROOT_SIGNATURE rglrs{};
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION rgAssoc{};
    const wchar_t* rgExports[1] = { L"RayGen" };
    if (raygenSrvs) {
        std::vector<D3D12_ROOT_PARAMETER> rps(raygenSrvs);
        for (UINT i = 0; i < raygenSrvs; ++i) {
            rps[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            rps[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            rps[i].Descriptor.ShaderRegister = i;
            rps[i].Descriptor.RegisterSpace = rq::kGeomIndexSpace;
        }
        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = raygenSrvs;
        rsd.pParameters = rps.data();
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT rhr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err);
        if (SUCCEEDED(rhr))
            rhr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&raygenRs));
        if (blob) blob->Release();
        if (err) err->Release();
        if (FAILED(rhr) || !raygenRs) {
            if (localRs) localRs->Release();
            *why = "cannot create the raygen local root signature of the shim's own layout";
            return false;
        }
        rglrs.pLocalRootSignature = raygenRs;
        rgAssoc.NumExports = 1;
        rgAssoc.pExports = rgExports;
    }
    auto releaseSigs = [&] {
        if (localRs) localRs->Release();
        if (raygenRs) raygenRs->Release();
    };

    std::vector<D3D12_STATE_SUBOBJECT> subs;
    // Reserved up front: the association points INTO this vector, so it must
    // never reallocate.
    subs.reserve(12 + hgs.size() + hgProcs.size());
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
    if (localRs) {
        subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lrs });
        assoc.pSubobjectToAssociate = &subs.back();
        subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
                         &assoc });
    }
    if (raygenRs) {
        subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &rglrs });
        rgAssoc.pSubobjectToAssociate = &subs.back();
        subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &rgAssoc });
    }

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
             "library, %u subobjects, %s, global root signature %p. If this is "
             "the last line in the log, that call is where the device went.\n",
             lib.size(), static_cast<unsigned>(subs.size()),
             localRs ? "record pairs through a local root SRV" : "no record data",
             (void*)globalRs);
    HRESULT hr = dev->CreateStateObject(&sod, IID_PPV_ARGS(&so));
    if (FAILED(hr)) {
        releaseSigs();
        char buf[96];
        std::snprintf(buf, sizeof(buf), "CreateStateObject failed (hr=0x%08X)",
                      static_cast<unsigned>(hr));
        *why = buf;
        return false;
    }

    ID3D12StateObjectProperties* props = nullptr;
    if (FAILED(so->QueryInterface(IID_PPV_ARGS(&props)))) {
        so->Release();
        releaseSigs();
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
        releaseSigs();
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
    out->localRs = localRs;
    out->raygenRs = raygenRs;
    return true;
}

}  // namespace

Dxr11RayQueryPso::~Dxr11RayQueryPso() {
    // Before anything else: the carrier is releasing us, so nothing may find
    // us through it again.
    UnregisterCarrier(this);
    for (ID3D12Resource* r : m_spare) r->Release();
    for (CachedTable& t : m_tables) t.sbt->Release();   // m_sbt is one of these
    for (OwnTable& t : m_ownTables) t.sbt->Release();
    if (m_localRs) m_localRs->Release();
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
//   rqphase = 4   all of it, dispatch included, as when absent
//   absent        all of it, the normal behaviour
//
// 4 differs from absent in one way only, and it is the reason it exists: a
// shader the rewriter REFUSES still gets a do-nothing pipeline, as under every
// phase, where absent forwards it and Unreal makes that fatal. So 4 is how the
// shaders that DO lower get to run in an Unreal game at all while the refusal
// list is still long. Rendering from the refused ones is missing, not wrong.
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
        if (s >= 4)
            ProxyLog("[dxr-tier-11-proxy-log] rqphase = %d (from %s): the FULL path, "
                     "lowered shaders are dispatched. Only a REFUSED shader gets a "
                     "pipeline that does nothing, so an Unreal game survives its "
                     "refusals. What the refused shaders draw is missing. This is a "
                     "diagnostic, not a setting.\n", s, t.source);
        else if (s >= 0)
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
    self->m_scenes = x.scenes;
    self->m_localRs = b.localRs;   // owned now, released in the destructor
    self->m_lib = lib;             // for the shim's own layout, built on need
    self->m_payloadBytes = x.payloadBytes;
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
             "%zu -> %zu bytes, numthreads(%u,%u,%u)%s%s%s\n",
             static_cast<size_t>(desc->CS.BytecodeLength), lib.size(),
             self->m_threads[0], self->m_threads[1], self->m_threads[2],
             x.needsBoth ? ", with a generated any-hit AND intersection shader"
                 : x.hasIntersection ? ", with a generated intersection shader"
                 : (x.hasAnyHit ? ", with a generated any-hit shader" : ""),
             x.appends ? ", which appends to a buffer per candidate" : "",
             x.carried ? ", carrying values read before the loop in the payload" : "");
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
    // Built for this exact layout before? The identifiers only change on a
    // rebake, which empties the cache.
    for (const CachedTable& t : m_tables) {
        if (t.kinds == kinds && t.pairs == recPairs) {
            m_sbt = t.sbt;
            m_kinds = kinds;
            m_recPairs = recPairs;
            m_desc = t.desc;
            return true;
        }
    }

    const UINT hitRecords =
        kinds.empty() ? 1u : static_cast<UINT>(kinds.size());

    // PADDED past what is known. The instance data arrives a submission late
    // on the GPU path, so the scene can reach records the shim has not read
    // yet, and an index past the end of the table is undefined. In Escher's
    // open world that is what hung the GPU. A padded record produces no hit:
    // missing for a frame, never undefined and never another record's answer.
    // Past kMinRecords, or twice what is known, it is still undefined.
    //
    // Rounded up to a multiple of kMinRecords so that tables for nearby
    // layouts are the same SIZE, and a spare one can be reused rather than a
    // new buffer created for every frame's layout.
    const UINT kMinRecords = 4096;
    const UINT tableRecords =
        AlignUp(hitRecords * 2 > kMinRecords ? hitRecords * 2 : kMinRecords, kMinRecords);

    std::vector<size_t> copyOf(hitRecords, 0);   // one hit group copy now

    // raygen, then miss, then the hit group records, each TABLE aligned to 64
    // and each RECORD to 32, then the pairs the records point at.
    //
    // With record data a hit record is the identifier followed by an 8-byte
    // GPU address: its local root SRV, pointing at ITS (geometry, contribution)
    // pair in the pairs region of this same buffer. Per record, so any number
    // of records and any number of distinct pairs; nothing is baked.
    const UINT argBytes = m_recordConstants ? 8u : 0u;
    const UINT stride = AlignUp(kIdSize + argBytes, kRecAlign);
    const UINT slot = AlignUp(kIdSize, kTableAlign);
    const UINT hitBytes = AlignUp(stride * tableRecords, kTableAlign);
    const UINT pairsOff = slot * 2 + hitBytes;
    const UINT pairsBytes = m_recordConstants ? tableRecords * 8u : 0u;
    const UINT64 size = static_cast<UINT64>(pairsOff) + pairsBytes;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // A spare of the right size that nothing can still read, or a new one.
    ID3D12Resource* sbt = nullptr;
    for (size_t i = 0; i < m_spare.size(); ++i) {
        if (m_spare[i]->GetDesc().Width != size || gpuhold::Busy(m_spare[i])) continue;
        sbt = m_spare[i];
        m_spare.erase(m_spare.begin() + i);
        dstats::Add(dstats::kTableRecycled);
        break;
    }
    if (!sbt && FAILED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
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
        // With baked record data, a record no known instance reaches has no
        // known pair: it gets the no-hit record rather than copy 0's answer.
        if (m_recordConstants && !kinds.empty() && reach == astrack::kReachNone)
            id = m_idNullTri;
        uint8_t* rec = p + 2 * slot + i * stride;
        std::memcpy(rec, id, kIdSize);
    }
    // The padding. A triangle-only shader with no record data answers the same
    // on every record, so its padding is its own record and an unknown index
    // is still right. Otherwise the rejecting triangle record: a triangle gets
    // IgnoreHit, a procedural primitive finds no intersection shader, and
    // neither produces a hit.
    const void* pad = (m_recordConstants || m_servesProc) ? m_idNullTri : m_idHit[0].data();
    for (UINT i = hitRecords; i < tableRecords; ++i)
        std::memcpy(p + 2 * slot + i * stride, pad, kIdSize);

    // Each record's pair, and the record's pointer to it. Padded records point
    // at zeros; their hit groups never read.
    if (m_recordConstants) {
        const D3D12_GPU_VIRTUAL_ADDRESS pairsVa = sbt->GetGPUVirtualAddress() + pairsOff;
        for (UINT i = 0; i < tableRecords; ++i) {
            if (i < hitRecords && i < recPairs.size()) {
                const uint32_t pair[2] = { recPairs[i].first, recPairs[i].second };
                std::memcpy(p + pairsOff + i * 8u, pair, sizeof(pair));
            }
            const D3D12_GPU_VIRTUAL_ADDRESS va = pairsVa + static_cast<UINT64>(i) * 8u;
            std::memcpy(p + 2 * slot + i * stride + kIdSize, &va, sizeof(va));
        }
    }
    sbt->Unmap(0, nullptr);

    m_sbt = sbt;
    m_kinds = kinds;
    m_recPairs = recPairs;

    const D3D12_GPU_VIRTUAL_ADDRESS base = sbt->GetGPUVirtualAddress();
    m_desc.RayGenerationShaderRecord.StartAddress = base + 0 * slot;
    m_desc.RayGenerationShaderRecord.SizeInBytes = kIdSize;
    m_desc.MissShaderTable.StartAddress = base + 1 * slot;
    // The miss record is the identifier alone. Its stride is its own, not
    // the hit records': with record data those are 64 bytes, and a 32-byte
    // table at stride 64 is invalid (debug layer #1161, 1731 times in the
    // 0.49.0 Escher run with the layer forced on; harmless on the 1070,
    // since the lowered raygen only ever uses miss index 0).
    m_desc.MissShaderTable.SizeInBytes = kIdSize;
    m_desc.MissShaderTable.StrideInBytes = kIdSize;
    m_desc.HitGroupTable.StartAddress = base + 2 * slot;
    m_desc.HitGroupTable.SizeInBytes = stride * tableRecords;
    m_desc.HitGroupTable.StrideInBytes = stride;

    CachedTable t;
    t.kinds = kinds;
    t.pairs = recPairs;
    t.sbt = sbt;
    t.desc = m_desc;
    m_tables.push_back(t);
    const size_t kMaxCachedTables = 8;
    if (m_tables.size() > kMaxCachedTables) {
        m_spare.push_back(m_tables.front().sbt);   // may still be in flight
        m_tables.erase(m_tables.begin());
    }
    // Keep the NEWEST few idle spares of the size just built, which is the
    // size the next table most likely needs, and release every other idle
    // one. 0.40.2 kept the OLDEST four whatever their size: in Escher those
    // were the menu's smaller tables, which never fit the open world's, so
    // every open-world table was a new buffer and the ones that would have
    // fitted were freed. A busy one is kept however many there are: something
    // may still read it.
    const size_t kIdleSpares = 4;
    size_t kept = 0;
    for (size_t i = m_spare.size(); i-- > 0;) {
        if (gpuhold::Busy(m_spare[i])) continue;
        if (m_spare[i]->GetDesc().Width == size && kept < kIdleSpares) {
            ++kept;
            continue;
        }
        m_spare[i]->Release();
        m_spare.erase(m_spare.begin() + i);
        dstats::Add(dstats::kTableFreed);
    }
    return true;
}

void Dxr11RayQueryPso::DispatchAsRays(ID3D12GraphicsCommandList4* cl,
                                      UINT gx, UINT gy, UINT gz,
                                      const std::vector<uint8_t>& recordKinds,
                                      const void* owner,
                                      const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* scenes) {
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
        const std::vector<astrack::RecordConstants> consts = astrack::RecordConstantsTable(scenes);
        recPairs.resize(recordKinds.size());
        for (size_t i = 0; i < recPairs.size() && i < consts.size(); ++i)
            recPairs[i] = { consts[i].geometryIndex, consts[i].instanceContribution };
    }

    // Rebuild when the scene's layout is not what the table was built for,
    // which is the normal case the first time: the acceleration structures did
    // not exist when the pipeline was created. A changed layout can mean more
    // records OR the same number with different types in them, or, with
    // record constants, records carrying different numbers.
    if (!recordKinds.empty() && (recordKinds != m_kinds || recPairs != m_recPairs)) {
        const size_t had = m_kinds.size();
        const bool wasCached = [&] {
            for (const CachedTable& t : m_tables)
                if (t.kinds == recordKinds && t.pairs == recPairs) return true;
            return false;
        }();
        std::string why;
        if (!BuildTable(recordKinds, recPairs, &why)) {
            ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch SKIPPED: the scene "
                     "needs %u hit group records and the table could not be "
                     "rebuilt (%s).\n",
                     static_cast<unsigned>(recordKinds.size()), why.c_str());
            dstats::Add(dstats::kSkippedTable);
            return;
        }
        dstats::Add(wasCached ? dstats::kTableCached : dstats::kTableNew);
        static LONG s_logged = 0;
        if (!wasCached && InterlockedIncrement(&s_logged) <= 32) {
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
    }

    // The state object and the table this dispatch uses, as one consistent
    // pair. The use is registered before the lock is released, so no other
    // thread can find the table idle and overwrite it in between.
    so = m_so;
    d = m_desc;
    gpuhold::Use(m_sbt, owner);
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
            dstats::Add(dstats::kHeldBack);
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
    dstats::Add(dstats::kDrawn);
}

// --- the shim's own record layout (0.61.0) -----------------------------------

namespace {

// What the retrace needs, for dxch::RewriteContainer.
struct RetraceCtx {
    std::vector<gidx::Scenes> slots;
    std::vector<unsigned> caps;
    unsigned m = 0;   // the lowered calls' multiplier: 1 with record data
};

bool RetraceLowered(const std::string& in, std::string* out, std::string* why, void* p) {
    const RetraceCtx* c = static_cast<const RetraceCtx*>(p);
    std::vector<gidx::Scenes> perCall;
    gidx::ScanCallScenes(in, &perCall);
    std::vector<unsigned> slotOf;
    for (const auto& call : perCall) {
        const int j = gidx::SlotOfCall(c->slots, call);
        if (j < 0) {
            *why = "a lowered TraceRay whose scene is not one of the shader's scene slots";
            return false;
        }
        slotOf.push_back((unsigned)j);
    }
    int calls = 0;
    const std::vector<std::pair<unsigned, unsigned>> pairs{ { 0u, c->m } };
    if (!rq::RetraceToShimScene(llm::Normalize(in), pairs, 0, slotOf, (unsigned)c->slots.size(),
                                c->caps, out, &calls, why))
        return false;
    if (!calls) {
        *why = "the lowered library has no TraceRay to point at the shim's scene";
        return false;
    }
    return true;
}

// DXR_TIER11_RQOWN_POISON=1: every record's pair takes the first instance's
// contribution, the sensitivity check for raytest --overlap. =2: every
// record gets the shader's own hit group whatever kind its geometry is, the
// check for --mixed, where the two kinds share the application's records.
int OwnPoison() {
    static const int p = [] {
        char b[4] = {};
        if (!GetEnvironmentVariableA("DXR_TIER11_RQOWN_POISON", b, sizeof(b))) return 0;
        return b[0] == '2' ? 2 : 1;
    }();
    return p;
}

}  // namespace

Dxr11RayQueryPso::Own::~Own() {
    if (so) so->Release();
    if (raygenRs) raygenRs->Release();
    if (localRs) localRs->Release();
}

bool Dxr11RayQueryPso::BuildOwn(const std::vector<UINT>& caps, std::shared_ptr<Own>* out,
                                std::string* why) {
    RetraceCtx ctx;
    ctx.slots = gidx::SceneSlots(m_scenes);
    ctx.caps.assign(caps.begin(), caps.end());
    ctx.m = m_recordConstants ? 1u : 0u;
    std::vector<uint8_t> lib;
    std::string err;
    if (!dxch::RewriteContainer(m_lib.data(), m_lib.size(), RetraceLowered, &ctx, &lib, &err)) {
        *why = "the lowered library could not be pointed at the shim's scene: " + err;
        return false;
    }
    auto own = std::make_shared<Own>();
    own->caps = caps;
    // One root SRV per sub-slot's copy, then the key table.
    const UINT srvs = own->Subs() + (own->Keyed() ? 1u : 0u);
    // The GTX 1070 removes its device past a local root signature size
    // (tier11/lrsprobe.cpp): 2 dwords a root descriptor, well under 192.
    if (2 * srvs > 160) {
        *why = "the shim's own layout would need a raygen local root signature of " +
               std::to_string(srvs) + " root descriptors, past what the GTX 1070 takes";
        return false;
    }
    Xform x;
    x.hasAnyHit = m_hasAnyHit;
    x.hasIntersection = m_hasIntersection;
    x.needsBoth = m_needsBoth;
    x.needsRecordConstants = m_recordConstants;
    x.payloadBytes = m_payloadBytes;
    Built b;
    if (!BuildStateObject(m_dev, lib, x, m_rootSig, &b, why, srvs)) return false;
    own->so = b.so;
    own->raygenRs = b.raygenRs;
    own->localRs = b.localRs;
    std::memcpy(own->idRay, b.idRay, kIdSize);
    std::memcpy(own->idMiss, b.idMiss, kIdSize);
    std::memcpy(own->idNullTri, b.idNullTri, kIdSize);
    std::memcpy(own->idNullProc, b.idNullProc, kIdSize);
    if (!b.idHit.empty()) own->idHit = b.idHit[0];
    if (!b.idHitProc.empty()) own->idHitProc = b.idHitProc[0];
    std::string held;
    if (own->Keyed()) {
        held = ", scenes per slot";
        for (UINT c : caps) held += " " + std::to_string(c);
    }
    ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery shader: the shim's OWN record layout "
             "built, %zu byte library, %u scene slot(s)%s\n", lib.size(), (unsigned)caps.size(),
             held.c_str());
    *out = own;
    return true;
}

bool Dxr11RayQueryPso::EnsureOwn(const std::vector<UINT>& need, std::shared_ptr<Own>* out,
                                 std::string* why) {
    auto covers = [&](const std::vector<UINT>& caps) {
        if (caps.size() != need.size()) return false;
        for (size_t j = 0; j < need.size(); ++j)
            if (need[j] > caps[j]) return false;
        return true;
    };
    if (m_own && covers(m_own->caps)) { *out = m_own; return true; }
    // A need at least as large as one that failed fails the same way.
    for (const auto& f : m_ownFailed) {
        bool atLeast = f.size() == need.size();
        for (size_t j = 0; atLeast && j < need.size(); ++j) atLeast = need[j] >= f[j];
        if (atLeast) { *why = m_ownWhy; return false; }
    }
    // Grown at least double, so a scene reaching more keys each frame does
    // not rebuild every frame; the exact need if the doubled one fails.
    std::vector<UINT> exact(need.size());
    for (size_t j = 0; j < need.size(); ++j) exact[j] = (std::max)(need[j], 1u);
    std::vector<UINT> caps = exact;
    for (size_t j = 0; m_own && j < need.size(); ++j) {
        const UINT had = j < m_own->caps.size() ? m_own->caps[j] : 1u;
        if (caps[j] > had) caps[j] = (std::max)(caps[j], had * 2);
    }
    std::shared_ptr<Own> own;
    std::string w;
    if (!BuildOwn(caps, &own, &w) && (exact == caps || !BuildOwn(exact, &own, &w))) {
        m_ownFailed.push_back(exact);
        m_ownWhy = w;
        *why = w;
        return false;
    }
    if (m_own) m_ownRetired.push_back(m_own);
    m_own = own;
    *out = own;
    return true;
}

bool Dxr11RayQueryPso::DispatchOwn(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev,
                                   UINT gx, UINT gy, UINT gz, const gidx::SceneSel& sel,
                                   const std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, UINT64>>* builds,
                                   const void* owner, const std::function<void()>& restore,
                                   std::string* why) {
    if (!cl || !dev) { *why = "no command list or device"; return false; }
    if (m_scenes.unknown) {
        *why = "a lowered TraceRay whose scene could not be traced back to a register or the heap";
        return false;
    }
    if (!sel.valid) {
        *why = sel.why.empty() ? "which scene each lowered TraceRay traces is not known" : sel.why;
        return false;
    }
    const UINT S = (UINT)gidx::SceneSlots(m_scenes).size();
    if (!S || sel.need.size() != S || sel.fixed.size() != sel.Subs() || sel.scenes.empty()) {
        *why = "the scene selection does not match the shader's scene slots";
        return false;
    }
    for (int f : sel.fixed)
        if (f < 0) { *why = "a scene slot not resolved through the root signature"; return false; }

    // Each scene as the build it is traced as: its instances, and their rows.
    // The copy and the records are made from the same build's.
    const UINT D = (UINT)sel.scenes.size();
    std::vector<std::vector<astrack::TlasInfo::Row>> rows(D);
    std::vector<std::vector<D3D12_RAYTRACING_INSTANCE_DESC>> descs(D);
    std::vector<bool> latest(D, true);
    std::vector<UINT64> buildOf(D, 0);
    for (UINT x = 0; x < D; ++x) {
        const D3D12_GPU_VIRTUAL_ADDRESS a = sel.scenes[x];
        const UINT64 now = astrack::LatestBuild(a);
        UINT64 b = now;
        if (builds)
            for (const auto& pb : *builds)
                if (pb.first == a) b = pb.second;
        buildOf[x] = b;
        latest[x] = b == now;
        if (b && astrack::InstancesAt(a, b, &descs[x], &rows[x])) continue;
        // Not kept (a copy of a structure the shim did not read, say): the
        // latest read, if it is the latest build.
        if (!latest[x] || !astrack::Current({ a }) || !astrack::Rows(a, &rows[x])) {
            *why = "the instances of the build a scene it traces had are not kept";
            return false;
        }
    }

    std::shared_ptr<Own> own;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (!EnsureOwn(sel.need, &own, why)) return false;
    }
    shimscene::Activate(1);   // later builds get their copy as they are built

    // A copy of every scene, one after another in the hit group table: scene
    // x's records start at base[x], instance i's at base[x] + i * gmax. An
    // empty scene has none and needs none: it is traced as it is, and every
    // ray misses. A scene built again since the dispatch was recorded is
    // copied from that build's instances.
    std::vector<shimscene::Copy> cps(D);
    std::vector<UINT> base(D + 1, 0);
    for (UINT x = 0; x < D; ++x) {
        std::string cw;
        if (rows[x].empty()) {
            cps[x].tlas = { sel.scenes[x] };
            cps[x].kc = 1;
            cps[x].gmax = 1;
            cps[x].base = base[x];
            base[x + 1] = base[x];
            continue;
        }
        const bool ok = latest[x] || descs[x].empty()
            ? shimscene::Ensure(cl, dev, sel.scenes[x], 1, base[x], owner, &cps[x], &cw)
            : shimscene::EnsureFrom(cl, dev, sel.scenes[x], buildOf[x], descs[x], 1, base[x], owner,
                                    &cps[x], &cw);
        if (!ok) {
            restore();
            *why = "no copy of a scene it traces: " + cw;
            return false;
        }
        for (const void* r : cps[x].tlasRes) gpuhold::Use(r, owner);
        gpuhold::Use(cps[x].contribRes, owner);
        const UINT64 end = (UINT64)base[x] + (UINT64)cps[x].count * cps[x].kc * cps[x].gmax;
        if (end > 0xFFFFFFull) {
            restore();
            *why = "the shim's own layout would need more than 2^24 hit group records";
            return false;
        }
        base[x + 1] = (UINT)end;
    }
    restore();

    // The instances of each scene, as its copy has them.
    for (UINT x = 0; x < D; ++x) {
        if (rows[x].size() != cps[x].count || cps[x].tlas.empty()) {
            *why = "the instances of a scene it traces are not read as its copy has them";
            return false;
        }
        for (const auto& r : rows[x]) {
            if (!r.active) continue;
            if (!r.geometries) {
                *why = "an instance on a bottom-level structure whose geometry the shim does not "
                       "know (deserialized, or never seen built)";
                return false;
            }
            if ((r.reach & astrack::kReachTriangles) && (r.reach & astrack::kReachProcedural)) {
                *why = "a bottom-level structure holding both triangles and procedural "
                       "primitives, which DXR does not allow";
                return false;
            }
            if (r.geometries > cps[x].gmax) {
                *why = "an instance with more geometries than its scene copy has room for";
                return false;
            }
        }
    }

    // The addresses the raygen record carries: sub-slot p of slot j names
    // the dispatch's sub-slot p, or past what it needs its first, which no
    // key picks; then the key table.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> addrs;
    for (UINT j = 0; j < S; ++j)
        for (UINT p = 0; p < own->caps[j]; ++p) {
            const UINT su = sel.Sub(j) + (p < sel.need[j] ? p : 0u);
            addrs.push_back(cps[(size_t)sel.fixed[su]].tlas[0]);
        }
    // The key table, as the variant's (0.60.0): dword j the start of slot
    // j's list, there n, then n (key, sub-slot) pairs sorted by key.
    static const bool keyPoison = GetEnvironmentVariableA("DXR_TIER11_KEY_POISON", nullptr, 0) != 0;
    std::vector<uint32_t> kt;
    if (own->Keyed()) {
        kt.assign(S, 0u);
        for (UINT j = 0; j < S; ++j) {
            kt[j] = (uint32_t)kt.size();
            kt.push_back((uint32_t)sel.keys[j].size());
            for (const auto& kp : sel.keys[j]) {
                kt.push_back(kp.first);
                kt.push_back(keyPoison ? 0u : kp.second);
            }
        }
    }

    // What the table is made from, to find one built before.
    std::vector<uint64_t> key;
    key.push_back((uint64_t)(uintptr_t)own.get());
    for (auto a : addrs) key.push_back(a);
    key.push_back(kt.size());
    for (uint32_t v : kt) key.push_back(v);
    for (UINT x = 0; x < D; ++x) {
        key.push_back(((uint64_t)cps[x].gmax << 32) | base[x]);
        for (const auto& r : rows[x])
            key.push_back(r.active ? (1ull << 63) | ((uint64_t)r.contribution << 24) |
                                         ((uint64_t)r.geometries << 8) | r.reach
                                   : 0ull);
    }

    ID3D12StateObject* so = nullptr;
    D3D12_DISPATCH_RAYS_DESC d{};
    {
    std::lock_guard<std::mutex> guard(m_lock);
    const OwnTable* found = nullptr;
    for (const OwnTable& t : m_ownTables)
        if (t.key == key) { found = &t; break; }
    if (found) {
        dstats::Add(dstats::kTableCached);
        d = found->desc;
        gpuhold::Use(found->sbt, owner);
    } else {
        // raygen (identifier and addresses), miss, the hit group records,
        // their pairs, then the key table.
        const UINT records = (std::max)(base[D], 1u);
        const UINT rgBytes = AlignUp(kIdSize + 8u * (UINT)(addrs.size() + (kt.empty() ? 0 : 1)),
                                     kRecAlign);
        const UINT rgSlot = AlignUp(rgBytes, kTableAlign);
        const UINT missSlot = AlignUp(kIdSize, kTableAlign);
        const UINT stride = AlignUp(kIdSize + (m_recordConstants ? 8u : 0u), kRecAlign);
        const UINT hitOff = rgSlot + missSlot;
        const UINT pairsOff = hitOff + AlignUp(stride * records, kTableAlign);
        const UINT pairsBytes = m_recordConstants ? records * 8u : 0u;
        const UINT keysOff = AlignUp(pairsOff + pairsBytes, 256);
        const UINT64 size = (UINT64)keysOff + kt.size() * 4u;

        ID3D12Resource* sbt = nullptr;
        for (size_t i = 0; i < m_spare.size(); ++i) {
            if (m_spare[i]->GetDesc().Width != size || gpuhold::Busy(m_spare[i])) continue;
            sbt = m_spare[i];
            m_spare.erase(m_spare.begin() + i);
            dstats::Add(dstats::kTableRecycled);
            break;
        }
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!sbt && FAILED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&sbt)))) {
            *why = "cannot create the shader table of the shim's own layout";
            return false;
        }
        uint8_t* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        if (FAILED(sbt->Map(0, &none, reinterpret_cast<void**>(&p)))) {
            sbt->Release();
            *why = "cannot map the shader table of the shim's own layout";
            return false;
        }
        const D3D12_GPU_VIRTUAL_ADDRESS va = sbt->GetGPUVirtualAddress();
        std::memset(p, 0, static_cast<size_t>(size));
        std::memcpy(p, own->idRay, kIdSize);
        for (size_t a = 0; a < addrs.size(); ++a) std::memcpy(p + kIdSize + 8 * a, &addrs[a], 8);
        if (!kt.empty()) {
            const D3D12_GPU_VIRTUAL_ADDRESS ka = va + keysOff;
            std::memcpy(p + kIdSize + 8 * addrs.size(), &ka, 8);
            std::memcpy(p + keysOff, kt.data(), kt.size() * 4);
        }
        std::memcpy(p + rgSlot, own->idMiss, kIdSize);

        // One record per (instance, geometry), of the kind its structure
        // holds: the shader's own group for a kind it serves, the one of
        // that kind that never commits for the other; nothing reaches the
        // rest, which get the rejecting triangle record.
        const int poison = OwnPoison();
        auto idFor = [&](uint8_t reach) -> const void* {
            if (poison == 2) return own->idHit.data();
            if (reach & astrack::kReachProcedural)
                return m_servesProc ? (m_servesTri ? own->idHitProc.data() : own->idHit.data())
                                    : own->idNullProc;
            if (reach & astrack::kReachTriangles)
                return m_servesTri ? own->idHit.data() : own->idNullTri;
            return own->idNullTri;
        };
        for (UINT r = 0; r < records; ++r) std::memcpy(p + hitOff + r * stride, own->idNullTri, kIdSize);
        for (UINT x = 0; x < D; ++x) {
            const UINT gmax = cps[x].gmax;
            const UINT first = rows[x].empty() ? 0u : rows[x][0].contribution;
            for (UINT i = 0; i < (UINT)rows[x].size(); ++i) {
                const auto& row = rows[x][i];
                if (!row.active) continue;
                for (UINT g = 0; g < row.geometries; ++g) {
                    const UINT r = base[x] + i * gmax + g;
                    std::memcpy(p + hitOff + r * stride, idFor(row.reach), kIdSize);
                    if (!m_recordConstants) continue;
                    const uint32_t pair[2] = { g, poison == 1 ? first : row.contribution };
                    std::memcpy(p + pairsOff + r * 8u, pair, sizeof(pair));
                }
            }
        }
        // Each record's pointer to its pair; records nothing reaches point
        // at zeros their hit groups never read.
        if (m_recordConstants)
            for (UINT r = 0; r < records; ++r) {
                const D3D12_GPU_VIRTUAL_ADDRESS pa = va + pairsOff + (UINT64)r * 8u;
                std::memcpy(p + hitOff + r * stride + kIdSize, &pa, sizeof(pa));
            }
        sbt->Unmap(0, nullptr);

        d.RayGenerationShaderRecord = { va, rgBytes };
        d.MissShaderTable = { va + rgSlot, kIdSize, kIdSize };
        d.HitGroupTable = { va + hitOff, (UINT64)stride * records, stride };
        OwnTable t;
        t.key = std::move(key);
        t.sbt = sbt;
        t.desc = d;
        t.own = own;
        m_ownTables.push_back(std::move(t));
        dstats::Add(dstats::kTableNew);
        gpuhold::Use(sbt, owner);
        if (m_ownTables.size() > 8) {
            m_spare.push_back(m_ownTables.front().sbt);   // may still be in flight
            m_ownTables.erase(m_ownTables.begin());
        }
        static LONG s_logged = 0;
        if (InterlockedIncrement(&s_logged) <= 16)
            ProxyLog("[dxr-tier-11-proxy-log] lowered RayQuery dispatch in the shim's own record "
                     "layout: %u scene(s), %u hit group records, one per (instance, geometry)\n",
                     D, records);
    }
    so = own->so;
    }   // m_lock

    d.Width = gx * m_threads[0];
    d.Height = gy * m_threads[1];
    d.Depth = gz * m_threads[2];
    cl->SetPipelineState1(so);
    cl->DispatchRays(&d);
    dstats::Add(dstats::kDrawn);
    dstats::Add(dstats::kOwnLayout);
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
