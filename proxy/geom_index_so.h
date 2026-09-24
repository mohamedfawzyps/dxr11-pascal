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
// Not yet built, and each refused BY NAME rather than drawn wrong: a layout
// where one record is reached by two geometries (a TraceRay multiplier of 0
// over a structure with several, for instance), TraceRay arguments computed
// at run time, hit groups coming from an EXISTING_COLLECTION, an indirect
// DispatchRays, and local root signatures associated from inside a library.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

// What DispatchRays needs, attached to the state object.
struct Info {
    std::vector<Group> groups;
    UINT recordBytes = 0;          // the largest record an extended group needs
    // The (RayContributionToHitGroupIndex, Multiplier...) pairs the
    // pipeline's TraceRay calls use, low 4 bits each.
    std::vector<std::pair<UINT, UINT>> traceArgs;
    bool dynamicTraceArgs = false; // a TraceRay whose R or M is not a constant
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
    std::deque<std::vector<LPCWSTR>> exportLists;
    std::deque<std::wstring> names;
    std::vector<Microsoft::WRL::ComPtr<ID3D12RootSignature>> sigs;
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
void Attach(ID3D12StateObject* so, const std::shared_ptr<Info>& info);
std::shared_ptr<Info> Get(ID3D12StateObject* so);

// Records the shim's copy of the application's hit group table into `cl`,
// and returns the range the dispatch should use instead. Replaces the
// compute root signature and pipeline; the CALLER restores them. `owner` is
// the recording list, for the buffers' lifetimes (gpu_hold).
bool RecordTable(ID3D12GraphicsCommandList4* cl, ID3D12Device* dev, const Info& info,
                 const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE& app,
                 D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE* shim,
                 const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& boundSrvs,
                 const void* owner, std::string* why);

}  // namespace gidx
