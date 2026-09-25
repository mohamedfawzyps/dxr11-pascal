// GeometryIndex() in an APPLICATION's own DXR 1.0 hit shaders (Tier 1.1).
//
// The GTX 1070's driver rejects a library that calls GeometryIndex(), and a
// DXR 1.0 hit shader has no other way to learn which geometry it serves: the
// only thing that differs between geometries is WHICH RECORD of the hit group
// table the hit runs from. So:
//
//   1. CreateStateObject. Every library that reads GeometryIndex() is
//      rewritten (proxy/rewriter/geom_index.h): the read becomes one 32-bit
//      constant at b0, space kGeomIndexSpace. The local root signature of
//      every hit group whose shaders read it gets that constant APPENDED, so
//      the application's own local arguments keep their offsets, and is
//      re-associated to exactly those hit groups and their shaders.
//   2. DispatchRays. The shim copies the application's hit group table into
//      a buffer of its own, at a stride with room for the constant, and
//      writes into each record of an extended hit group the geometry index a
//      hit there has. Which geometry lands on which record follows from the
//      instances (as_tracker) and the TraceRay arguments the libraries use,
//      read out of their disassembly. The application's table is never
//      written; the dispatch uses the copy.
//
//   3. A record several geometries reach: a VARIANT pipeline in the shim's
//      own record layout, tracing the shim's copy of the scene (0.46.0).
//      Which scene a dispatch traces is resolved through its root signature,
//      root SRV or descriptor table (ResolveScenes, 0.47.0), or a heap index
//      in root constants or a CPU-visible root CBV (0.48.0).
//
// An indirect DispatchRays gets the same treatment, at record time when the
// arguments are CPU-visible and at submit when the command list is split
// (0.50.0).
//
// Subobjects a rewritten library declares itself are declared again at state
// object scope, with the spec's association rules (0.50.0).
//
// A scene not read yet, or read from an older build than its latest (GPU-
// written instances are read every few builds, a submission late), goes to
// the variant, which needs no CPU read; its copy of the scene is made from
// the instances the shim saved at that build (0.51.0).
//
// Not yet built, and each refused BY NAME rather than drawn wrong: TraceRay
// arguments computed at run time, TraceRay in a closest-hit or miss in the
// shim's layout, a rewritten library with its own subobjects included through
// an export list, a library association to another library's subobject. A scene not resolved (a heap index in GPU-only
// memory or a descriptor table, a local root signature) is judged over every
// live scene, which can refuse but not draw wrong.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "trace_args.h"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class StateObjectStore;

namespace gidx {

// Every root signature the application creates: its blob is kept on the
// object, so an extended copy of a LOCAL one can be made later.
void NoteRootSignature(ID3D12RootSignature* rs, const void* blob, size_t size);

// A hit group whose records carry the geometry index.
struct Group {
    std::wstring name;
    UINT offset = 0;               // byte offset of the constant in the record
    uint8_t id[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = {};
};

// An identifier the variant pipeline changed. `insert`, when nonzero, is
// where in the record the shim's appended root descriptors go: the scene
// copy, or with arguments computed at run time the copies and the pair table
// (Info::variantCopies).
struct Remap {
    uint8_t from[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = {};
    uint8_t to[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = {};
    UINT insert = 0;
};

// Where a library's TraceRay calls take their scene from, so a dispatch can be
// told WHICH scene it traces (ResolveScenes). `regs`: declared registers,
// `count` 0 an unbounded array. `heap`: ResourceDescriptorHeap[i] (SM 6.6,
// Unreal's bindless), `i` a dword of the cbuffer at (space, reg), byte
// `offset`. `unknown`: a TraceRay whose scene handle could not be traced back
// to either.
struct Scenes {
    struct Reg { UINT space = 0, lower = 0, count = 1; };
    struct Heap { UINT space = 0, reg = 0, offset = 0; };
    std::vector<Reg> regs;
    std::vector<Heap> heap;
    bool unknown = false;
};

// Adds the scenes of every TraceRay in one disassembled library to `*out`.
// Used for the application's own DXR libraries and for lowered RayQuery ones.
void ScanScenes(const std::string& text, Scenes* out);

// The local root signature of a raygen, miss or hit group, found by the
// identifier its records start with: where a scene bound through it sits in
// a record (0.55.0 raygens, 0.56.0 all three).
struct RecordLocal {
    uint8_t id[D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES] = {};
    uint32_t kind = 0;                            // 7 raygen, 11 miss, 3 hit group
    std::string why;                              // nonempty: its signature cannot be told
    Microsoft::WRL::ComPtr<ID3D12VersionedRootSignatureDeserializer> des;
    const D3D12_ROOT_SIGNATURE_DESC1* desc = nullptr;   // null: it has none
};

// What DispatchRays needs, attached to the state object.
struct Info {
    std::vector<Group> groups;
    UINT recordBytes = 0;          // the largest record an extended group needs
    // The (RayContributionToHitGroupIndex, Multiplier...) pairs the
    // pipeline's TraceRay calls can use, low 4 bits each, every cbuffer dword
    // taken as unknown; `args` gives the ones a dispatch uses (0.57.0).
    std::vector<std::pair<UINT, UINT>> traceArgs;
    targs::Args args;
    bool dynamicTraceArgs = false; // a library that could not be read
    Scenes scenes;                 // where the TraceRay calls take their scene

    // The shim's own record layout, for when the application's shares a
    // record between geometries: a VARIANT pipeline tracing the shim's copy
    // of the scene (proxy/shim_scene.h). Built from `store`, a copy of the
    // object's (transformed) subobjects, eagerly when a TraceRay multiplier
    // is 0, otherwise at the first dispatch that needs it.
    D3D12_STATE_OBJECT_TYPE type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    std::shared_ptr<StateObjectStore> store;
    std::mutex variantLock;
    bool variantTried = false;
    std::string variantWhy;
    Microsoft::WRL::ComPtr<ID3D12StateObject> variant;
    std::vector<Microsoft::WRL::ComPtr<ID3D12StateObject>> variantParts;  // its collections
    std::vector<Remap> remaps;
    UINT raygenBytes = 0;          // the largest raygen record the variant needs
    // The largest hit group and miss records the variant needs: a closest-hit
    // or miss that traces gets the shim scene's address too (0.56.0).
    UINT variantHitBytes = 0, variantMissBytes = 0;
    // 0: every call's pair a literal, traced as (its index, k). N >= 1: the
    // pair read at run time from a table the shim writes per dispatch, onto
    // one of N scene copies of 15 pairs each (0.57.0).
    UINT variantCopies = 0;

    // Every record's local root signature, found at the first dispatch that
    // needs one, and the pipeline's recursion depth.
    std::mutex localLock;
    bool localTried = false;
    std::string localWhy;
    std::vector<RecordLocal> recordLocals;
    UINT recursion = 0;
};

// Owns everything the transformed desc points at.
struct Transformed {
    D3D12_STATE_OBJECT_DESC desc{};
    std::shared_ptr<Info> info;
    std::vector<D3D12_STATE_SUBOBJECT> subs;
    std::deque<std::vector<uint8_t>> code;
    std::deque<D3D12_DXIL_LIBRARY_DESC> libs;
    std::deque<D3D12_LOCAL_ROOT_SIGNATURE> lrs;
    std::deque<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> assoc;
    std::deque<D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION> dxilAssoc;
    std::deque<D3D12_EXISTING_COLLECTION_DESC> colls;
    std::deque<std::vector<LPCWSTR>> exportLists;
    std::deque<std::wstring> names;
    std::vector<Microsoft::WRL::ComPtr<ID3D12RootSignature>> sigs;
    std::deque<std::vector<uint8_t>> raw;   // other subobject descs, by value
    std::string summary;           // for the log
};

enum class Outcome {
    kNothing,       // no library reads GeometryIndex(): forward as it was
    kTransformed,   // create `out->desc` instead, then Attach
    kRefused,       // reads it, and this cannot serve it yet: *why says why
};

// `dev` is the REAL device, used for the extended root signatures.
Outcome Transform(ID3D12Device* dev, const D3D12_STATE_OBJECT_DESC& in,
                  Transformed* out, std::string* why);

// After a successful create: reads the identifiers and attaches the info.
// Keeps a copy of the subobjects when a variant may be needed, and builds it
// at once when a TraceRay multiplier of 0 makes it certain to be.
void Attach(ID3D12Device* dev, ID3D12StateObject* so, const D3D12_STATE_OBJECT_DESC& desc,
            const std::shared_ptr<Info>& info);
std::shared_ptr<Info> Get(ID3D12StateObject* so);

// One root argument as bound at the dispatch: a root SRV's address, or a
// descriptor table's GPU handle, 0 when the parameter is neither.
struct BoundRoot {
    D3D12_GPU_VIRTUAL_ADDRESS srv = 0, cbv = 0;
    UINT64 table = 0;
    const UINT* constants = nullptr;   // root constants as set, and how many
    UINT numConstants = 0;
};

// The scenes the dispatch's TraceRay calls trace, resolved through the bound
// global root signature: a root SRV, or a descriptor table entry the shim saw
// a structure written to (proxy/scene_bind.h); or for a heap-indexed scene,
// the index read from root constants or a root CBV in CPU-visible memory at
// record time, and that heap slot. True, with `*out` exactly those
// addresses, when every register resolves; false with *why otherwise.
//
// A register the global root signature does not declare may be in the
// RAYGEN's local one: with `local`, its record is read for it (0.55.0).
// Without, `*needsLocal` says that is what failed.
struct LocalRecord {
    const D3D12_ROOT_SIGNATURE_DESC1* desc = nullptr;  // the record's local root signature
    const uint8_t* record = nullptr;                   // the record, identifier first
    UINT size = 0;
    // A register in neither signature is skipped, not a failure: the record
    // is a miss's or hit group's, whose shader then cannot use it (creating
    // the pipeline fails for a register no signature declares).
    bool lenient = false;
};
bool ResolveScenes(const Scenes& sc, ID3D12RootSignature* rs, const std::vector<BoundRoot>& roots,
                   ID3D12DescriptorHeap* const* heaps, UINT numHeaps,
                   std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* out, std::string* why,
                   const LocalRecord* local = nullptr, bool* needsLocal = nullptr);

// Finds, once, the local root signature of every raygen, miss and hit group
// of the pipeline `app` (whose Info this is), through linked collections and
// their renames. `*recursion`: the pipeline's depth; above 1 a closest-hit or
// miss can trace, through its own records.
bool PrepareRecordLocals(Info& info, ID3D12StateObject* app, UINT* recursion, std::string* why);
// The one whose identifier is `id`, null when none is.
const RecordLocal* RecordLocalOf(Info& info, const uint8_t* id);

// The (R, M) pairs a dispatch's TraceRay calls can use: the cbuffer dwords
// their arguments are computed from read from the bound global root
// signature (root constants, or a root CBV the CPU can read, at record time),
// and for a raygen's calls from its record's local one when `raygen` is given.
// A dword that cannot be read counts as every value.
std::vector<std::pair<UINT, UINT>> TracePairs(const Info& info, ID3D12RootSignature* rs,
                                              const std::vector<BoundRoot>& roots,
                                              const LocalRecord* raygen);

// Records the shim's copy of the application's hit group table into `cl`,
// and returns the range the dispatch should use instead. Replaces the
// compute root signature and pipeline; the CALLER restores them. `owner` is
// the recording list, for the buffers' lifetimes (gpu_hold).
// `boundSrvs` are the scenes the dispatch may trace; `exact` when they are
// exactly the ones it does (ResolveScenes), otherwise the bound root SRVs.
// `*shared` is set when it failed because the application's layout shares a
// record between geometries, which the variant serves.
// `pairs`: the (R, M) pairs this dispatch's calls can use (TracePairs).
bool RecordTable(ID3D12GraphicsCommandList4* cl, ID3D12Device* dev, const Info& info,
                 const std::vector<std::pair<UINT, UINT>>& pairs,
                 const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE& app,
                 D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE* shim,
                 const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& boundSrvs, bool exact,
                 const void* owner, bool* shared, std::string* why);

// The variant, built once; false with *why when it cannot be.
bool EnsureVariant(ID3D12Device* dev, Info& info, ID3D12StateObject* app, std::string* why);

// Records the variant's four tables, the hit group table in the shim's own
// layout, into `cl`, and fills `mine` with them. The caller binds the
// variant, dispatches, and binds the application's pipeline again.
bool RecordVariant(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, Info& info,
                   const std::vector<std::pair<UINT, UINT>>& pairs,
                   const D3D12_DISPATCH_RAYS_DESC& app, D3D12_DISPATCH_RAYS_DESC* mine,
                   const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& boundSrvs, bool exact,
                   const void* owner, std::string* why);

}  // namespace gidx
