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

#include <cstdint>
#include <string>
#include <vector>

#include "as_tracker.h"

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
    // The table is rebuilt whenever this changes, not merely when it grows:
    // a slot can go from holding the real hit group to holding a rejecting one
    // without the count moving at all.
    void DispatchAsRays(ID3D12GraphicsCommandList4* cl, UINT gx, UINT gy, UINT gz,
                        const std::vector<uint8_t>& recordKinds);

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
    bool BuildTable(const std::vector<uint8_t>& kinds,
                    const std::vector<astrack::RecordConstants>& consts,
                    std::string* why);

    ID3D12Device5* m_dev = nullptr;
    ID3D12StateObject* m_so = nullptr;
    ID3D12RootSignature* m_rootSig = nullptr;   // the app's, kept alive
    // Ours: two root constants carrying the geometry index and the
    // instance contribution into every hit group record.
    ID3D12RootSignature* m_localRootSig = nullptr;
    bool m_recordConstants = false;
    ID3D12Resource* m_sbt = nullptr;            // raygen, miss, hit, one buffer
    // Tables replaced by a growth. A dispatch recorded against the old one may
    // still be in flight, and nothing here knows when it lands, so they are
    // held until the pipeline itself dies. Growth is monotonic and rare, so
    // this stays a handful of small buffers.
    std::vector<ID3D12Resource*> m_retired;
    // Which geometry kinds the lowered shader actually commits. Both can be
    // true, and that is the case with two real hit groups.
    bool m_servesTri = false;
    bool m_servesProc = false;
    // The identifiers, kept so the table can be rebuilt without going back to
    // the state object. m_idNullTri and m_idNullProc are the hit groups that
    // never commit, one per geometry type.
    uint8_t m_idRay[32]{}, m_idMiss[32]{}, m_idHit[32]{};
    uint8_t m_idNullTri[32]{}, m_idNullProc[32]{};
    // Only a both-kinds shader has a second REAL hit group: one of each type,
    // with its own closest-hit, because the committed status differs.
    uint8_t m_idHitProc[32]{};
    // What the current table was built for, so a rebuild happens when the
    // scene's layout changes and not otherwise.
    std::vector<uint8_t> m_kinds;
    D3D12_DISPATCH_RAYS_DESC m_desc{};
    UINT m_threads[3] = { 1, 1, 1 };
    LONG m_refs = 1;
};
