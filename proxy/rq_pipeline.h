// The dispatch path: making a lowered RayQuery compute shader actually run.
//
// Rewriting the DXIL is half the job. The application created a COMPUTE
// pipeline and will call Dispatch; the lowered shader is a raytracing LIBRARY
// and needs DispatchRays against a state object and a shader table. Nothing
// the application does changes, so the shim has to own that machinery.
//
// The trick that makes this tractable: **DXR's global root signature IS the
// compute root signature.** SetComputeRootSignature and SetComputeRoot*View
// are exactly what DispatchRays consumes, so the application's bindings carry
// across untouched. Only the pipeline object and the dispatch call are
// substituted.
//
// Dxr11RayQueryPso is that substitution: an ID3D12PipelineState the
// application holds and never inspects, standing in for a state object and
// shader table it knows nothing about. The same shape as Dxr11CommandSignature
// in the indirect DispatchRays work, and for the same reason.
#pragma once

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "as_tracker.h"
#include "geom_index_so.h"

namespace rq {
// A hit group record's (geometryIndex, instanceContribution).
using RecordPair = std::pair<uint32_t, uint32_t>;
}

// Private IID, so a pipeline state can be recognised as ours through
// QueryInterface without ever being confused for a real one.
// {7E3B1C42-9A54-4D18-8F60-2C71B0A4E9D3}
extern const GUID IID_Dxr11RayQueryPso;

// The rqphase bisect, exposed so a REFUSED shader can be given a do-nothing
// pipeline too while a phase is set.
//
// Without that the phases are not comparable. rqphase = 1 ended with Unreal's
// "Shader compilation failures are Fatal" rather than a GPU crash, which looked
// like the rewrite being exonerated and was not: a forwarded refusal kills an
// Unreal run early, so that phase may never have reached the point where the
// driver was dying. rqlimit = 0 had the identical flaw.
//
// -1 when unset.
int Dxr11RayQueryPhase();

// Say what happened to one shader, but only while a phase is set.
//
// A phase substitutes a do-nothing pipeline for BOTH outcomes, so the log went
// silent per shader and a whole run became uninterpretable: it could not say
// whether the single RayQuery shader in it had lowered and built a state
// object, or been refused. The crash/no-crash bit is worthless without that.
void Dxr11RayQueryPhaseNote(const char* what, const char* detail);

class Dxr11RayQueryPso : public ID3D12PipelineState {
public:
    // Build everything a lowered RayQuery compute shader needs to run, or
    // explain why not. `why` is filled on failure and the caller forwards the
    // original creation unchanged, so a refusal costs the application nothing
    // beyond the log line.
    // Returns the CARRIER: a real compute pipeline state, created from a
    // do-nothing shader and the application's own root signature, with this
    // object attached to it as private data.
    //
    // It used to return the stand-in itself, and the application held an
    // ID3D12PipelineState that D3D12 never made. Every call that can carry one
    // back to the runtime then had to be found and trapped, and they were
    // found one crash at a time: SetPipelineState, then Reset, ClearState and
    // CreateCommandList, then StorePipeline. Each fix was correct and none of
    // them was the last one, because there is no list of every place a
    // pipeline state can go.
    //
    // A real object has no such list to get wrong. The application can reset
    // with it, name it, cache it, serialise it, release it; all of that is
    // D3D12 handling its own object. This shim only has to notice, at
    // dispatch, that the object carries a lowered query.
    static ID3D12PipelineState* TryCreate(ID3D12Device5* dev,
                                          const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                          std::string* why);

    // The lowered query attached to a pipeline state, or null for an ordinary
    // one. A pointer lookup, so it costs nothing on the overwhelmingly common
    // path where the answer is no.
    static Dxr11RayQueryPso* From(ID3D12PipelineState* p);

    static void RegisterCarrier(ID3D12PipelineState* carrier, Dxr11RayQueryPso* self);
    static void UnregisterCarrier(Dxr11RayQueryPso* self);

    // True when the lowered shader commits PROCEDURAL hits. That decides
    // whether the scene's geometry can be served by it, which the caller asks
    // the acceleration structure tracker about. A shader that commits BOTH
    // kinds says true here as well: the slot it cannot serve is one holding
    // both kinds at once, whichever else it handles.
    bool CommitsProcedural() const { return m_servesProc; }

    // Where the lowered library's TraceRay calls take their scene from, read
    // off the lowered text, so a dispatch can be judged against the ONE scene
    // it traces (gidx::ResolveScenes) instead of every live one.
    const gidx::Scenes& Scenes() const { return m_scenes; }

    // Issue the work the application asked for as Dispatch. The ray grid is
    // the thread group count times the shader's numthreads, because the
    // lowered raygen reads DispatchRaysIndex where the original read
    // SV_DispatchThreadID.
    //
    // `recordKinds` says what geometry reaches each hit group record index,
    // one entry per index, using astrack's Reach bits. Its SIZE is how many
    // records the scene needs. The caller supplies it because it comes from
    // the acceleration structures, which this class knows nothing about.
    //
    // Empty means nothing has been read yet, and one record will do.
    //
    // `scenes`, when the dispatch's scene was resolved, are exactly the
    // structures it traces, and the record constants come from them alone;
    // null means every live structure.
    //
    // The table is rebuilt whenever this changes, not merely when it grows:
    // a slot can go from holding the real hit group to holding a rejecting one
    // without the count moving at all.
    void DispatchAsRays(ID3D12GraphicsCommandList4* cl, UINT gx, UINT gy, UINT gz,
                        const std::vector<uint8_t>& recordKinds,
                        const void* owner,
                        const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* scenes = nullptr);

    // The same work in the SHIM'S OWN record layout (0.61.0), for a scene
    // whose layout the table above cannot serve: a record two instances, or
    // two live structures, disagree about, or triangles and procedural
    // primitives on one record. Until 0.61.0 all three were refused and
    // drew nothing.
    //
    // A second state object, built on first need, whose raygen traces the
    // shim's copy of each scene (proxy/shim_scene.h) instead of the
    // application's: the lowered TraceRay calls are retraced exactly as a
    // GeometryIndex() variant's are (rq::RetraceToShimScene), their scenes
    // in root SRVs of a raygen local root signature. In a copy instance i
    // sits at contribution base + i * gmax, so every (instance, geometry)
    // has a record of its own, of the one kind its bottom-level structure
    // holds, carrying its own (geometry, contribution) pair. Nothing can
    // collide.
    //
    // `sel`: which scene each scene slot traces (the command list's
    // ScenesSel), keys included; a slot holding several scenes grows the
    // state object on demand, as the variant does. `builds`, at submit: the
    // build each scene had when the dispatch was recorded, (scene, serial),
    // which may have been built again since (the copy is then made from that
    // build's instances, astrack::InstancesAt); null, its latest. `restore`
    // gives the application's compute bindings back after the copies are
    // recorded. False with *why, nothing recorded but possibly copies, when
    // it cannot.
    bool DispatchOwn(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, UINT gx, UINT gy, UINT gz,
                     const gidx::SceneSel& sel,
                     const std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, UINT64>>* builds,
                     const void* owner, const std::function<void()>& restore, std::string* why);

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // --- ID3D12Object ---
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override;
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override;

    // --- ID3D12DeviceChild ---
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** pp) override;

    // --- ID3D12PipelineState ---
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** pp) override;

private:
    Dxr11RayQueryPso() = default;

    // Creation order, used only by the rqdispatch bisect below.
    int m_index = 0;
    ~Dxr11RayQueryPso();

    // (Re)build the shader table so that each record matches the geometry
    // that reaches it. The shim has one REAL hit group, of one type, plus two
    // that never commit, one of each type. A slot reached by the kind the real
    // group serves gets the real group; a slot reached only by the other kind
    // gets the rejecting group OF THAT KIND, so the geometry is traversed with
    // a record of the correct type and simply produces no hit.
    //
    // With record data, each record also carries a pointer, a local root SRV,
    // to its own (geometryIndex, contribution) pair in the same buffer.
    bool BuildTable(const std::vector<uint8_t>& kinds,
                    const std::vector<rq::RecordPair>& recPairs,
                    std::string* why);

    // The shim's own layout: its state object at some capacity per scene
    // slot, built from the lowered library kept here.
    struct Own {
        ID3D12StateObject* so = nullptr;
        ID3D12RootSignature* raygenRs = nullptr;   // the copies' root SRVs, and the key table
        ID3D12RootSignature* localRs = nullptr;    // the hit groups' pair, as m_localRs
        uint8_t idRay[32]{}, idMiss[32]{}, idNullTri[32]{}, idNullProc[32]{};
        std::array<uint8_t, 32> idHit{}, idHitProc{};
        std::vector<UINT> caps;                    // scenes per slot
        bool Keyed() const { for (UINT c : caps) if (c > 1) return true; return false; }
        UINT Subs() const { UINT s = 0; for (UINT c : caps) s += c; return s; }
        ~Own();
    };
    bool EnsureOwn(const std::vector<UINT>& need, std::shared_ptr<Own>* out, std::string* why);
    bool BuildOwn(const std::vector<UINT>& caps, std::shared_ptr<Own>* out, std::string* why);
    std::vector<uint8_t> m_lib;                    // the lowered library
    UINT m_payloadBytes = 0;
    std::shared_ptr<Own> m_own;
    std::vector<std::shared_ptr<Own>> m_ownRetired;   // grown out of; lists may hold them
    std::vector<std::vector<UINT>> m_ownFailed;       // capacities that did not build
    std::string m_ownWhy;
    struct OwnTable {
        std::vector<uint64_t> key;
        ID3D12Resource* sbt = nullptr;
        D3D12_DISPATCH_RAYS_DESC desc{};
        std::shared_ptr<Own> own;
    };
    std::vector<OwnTable> m_ownTables;

    ID3D12Device5* m_dev = nullptr;
    ID3D12StateObject* m_so = nullptr;
    ID3D12RootSignature* m_rootSig = nullptr;   // the app's, kept alive
    // Does the shader ask what geometry or instance contribution it hit? Then
    // each hit record points at its pair through a LOCAL ROOT SRV, read with
    // rawBufferLoad. Not a cbuffer: a hit shader reading a cbuffer through the
    // local root signature crashes the Pascal driver, see
    // phase5/cases/driver-crash/README.md.
    bool m_recordConstants = false;
    gidx::Scenes m_scenes;
    ID3D12RootSignature* m_localRs = nullptr;   // owned; null without record data
    // What the current table's records point at, one pair per record.
    std::vector<rq::RecordPair> m_recPairs;
    // A dispatch can rebuild the table, and command lists are recorded on
    // several threads.
    std::mutex m_lock;
    bool m_hasAnyHit = false;
    bool m_hasIntersection = false;
    bool m_needsBoth = false;
    ID3D12Resource* m_sbt = nullptr;            // raygen, miss, hit, one buffer
    // Tables already built, per scene layout, OWNING their buffers; m_sbt
    // points into one of them. An engine can alternate between two layouts
    // every frame, and Escher did: 0.39.1 built a new table on every flip,
    // 1095 in one run, and freed none. Evicted tables go to m_spare, since a
    // dispatch may still be using them.
    struct CachedTable {
        std::vector<uint8_t> kinds;
        std::vector<rq::RecordPair> pairs;
        ID3D12Resource* sbt = nullptr;
        D3D12_DISPATCH_RAYS_DESC desc{};
    };
    std::vector<CachedTable> m_tables;
    // Evicted tables. A dispatch recorded against one may still be in flight,
    // so each is reused or released only once gpuhold says nothing can read
    // it any more. Until 0.40.2 they were held until the pipeline died, and
    // Escher's open world, a new layout nearly every frame, piled up about
    // 3 GB of them in 7 minutes.
    std::vector<ID3D12Resource*> m_spare;
    // Which geometry kinds the lowered shader actually commits. Both can be
    // true, and that is the case with two real hit groups.
    bool m_servesTri = false;
    bool m_servesProc = false;
    // The identifiers, kept so the table can be rebuilt without going back to
    // the state object. m_idNullTri and m_idNullProc are the hit groups that
    // never commit, one per geometry type.
    uint8_t m_idRay[32]{}, m_idMiss[32]{};
    uint8_t m_idNullTri[32]{}, m_idNullProc[32]{};
    // The REAL hit group, one per baked pair (exactly one when there is no
    // record constant). Only a both-kinds shader has a second real group per
    // pair: one of each type, with its own closest-hit, because the committed
    // status differs.
    std::vector<std::array<uint8_t, 32>> m_idHit;
    std::vector<std::array<uint8_t, 32>> m_idHitProc;
    // What the current table was built for, so a rebuild happens when the
    // scene's layout changes and not otherwise.
    std::vector<uint8_t> m_kinds;
    D3D12_DISPATCH_RAYS_DESC m_desc{};
    UINT m_threads[3] = { 1, 1, 1 };
    LONG m_refs = 1;
};
