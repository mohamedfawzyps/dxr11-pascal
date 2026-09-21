// Phase 4 - deep copy of a D3D12_STATE_OBJECT_DESC, and the per-state-object
// cache that AddToStateObject emulation is built on.
//
// Why this exists. On Tier 1.0 the driver refuses a state object that carries
// D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS:
//
//     ID3D12Device::CreateStateObject: Invalid D3D12_STATE_OBJECT_FLAGS: 0x4
//
// so AddToStateObject can never be reached, never mind succeed. The shim strips
// that flag on the way through, keeps a copy of everything the app passed, and
// when AddToStateObject is called rebuilds the whole object from the cached
// subobjects plus the addition. See docs/phase4-probe.md.
//
// The copy is the hard part. A D3D12_STATE_OBJECT_DESC is a flat array of
// subobjects, but each one points at a type-specific desc that in turn points at
// wide strings, shader bytecode, export arrays and COM objects, none of which
// the caller has to keep alive after the call returns. Worse,
// SUBOBJECT_TO_EXPORTS_ASSOCIATION holds a pointer INTO the same subobject
// array, so a naive copy leaves it aimed at the caller's stack. Those pointers
// are remapped by index in Finalize().

#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// An owning copy of a set of state subobjects. Append can be called more than
// once, which is what merges a cached base with an addition.
class StateObjectStore {
public:
    // Appends every subobject in `desc`. Returns false and fills `why` if a
    // subobject type is not supported, rather than silently dropping it: a
    // dropped subobject would produce a state object that builds but behaves
    // differently, which is the worst outcome for this project.
    //
    // `dropDuplicateSingletons` skips subobject types that may appear only once
    // per state object when one is already present. An addition is required to
    // repeat the configs (measured, see docs/phase4-probe.md), so merging
    // without this would produce duplicates.
    bool Append(const D3D12_STATE_OBJECT_DESC& desc,
                bool dropDuplicateSingletons,
                std::string& why);

    // True if any STATE_OBJECT_CONFIG carries ALLOW_STATE_OBJECT_ADDITIONS.
    bool HasAdditionsFlag() const;
    // Clears that flag wherever it appears. This is what makes a Tier 1.0
    // driver accept the object.
    void StripAdditionsFlag();

    // Resolves association pointers and returns a desc valid for as long as this
    // store is alive and unmodified.
    D3D12_STATE_OBJECT_DESC Desc(D3D12_STATE_OBJECT_TYPE type);

    size_t Count() const { return m_subs.size(); }

private:
    LPCWSTR Intern(LPCWSTR s);            // null-safe, returns stable storage
    const D3D12_EXPORT_DESC* CopyExports(const D3D12_EXPORT_DESC* p, UINT n);
    LPCWSTR* CopyNames(const LPCWSTR* p, UINT n);
    bool Has(D3D12_STATE_SUBOBJECT_TYPE t) const;

    // Every container here must keep existing elements at stable addresses as it
    // grows, because the subobject array points into them. deque guarantees
    // that; vector does not.
    std::deque<D3D12_STATE_OBJECT_CONFIG>                    m_cfg;
    std::deque<D3D12_GLOBAL_ROOT_SIGNATURE>                  m_globalRs;
    std::deque<D3D12_LOCAL_ROOT_SIGNATURE>                   m_localRs;
    std::deque<D3D12_NODE_MASK>                              m_nodeMask;
    std::deque<D3D12_DXIL_LIBRARY_DESC>                      m_lib;
    std::deque<D3D12_EXISTING_COLLECTION_DESC>               m_collection;
    std::deque<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION>       m_assoc;
    std::deque<D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION>  m_dxilAssoc;
    std::deque<D3D12_RAYTRACING_SHADER_CONFIG>               m_shaderCfg;
    std::deque<D3D12_RAYTRACING_PIPELINE_CONFIG>             m_pipeCfg;
    std::deque<D3D12_RAYTRACING_PIPELINE_CONFIG1>            m_pipeCfg1;
    std::deque<D3D12_HIT_GROUP_DESC>                         m_hitGroup;

    std::deque<std::vector<uint8_t>>          m_blobs;        // shader bytecode
    std::deque<std::wstring>                  m_strings;      // interned names
    std::deque<std::vector<D3D12_EXPORT_DESC>> m_exportArrays;
    std::deque<std::vector<LPCWSTR>>          m_nameArrays;
    std::vector<Microsoft::WRL::ComPtr<IUnknown>> m_keepAlive; // root sigs, collections

    // Working array, one entry per appended subobject. A dropped duplicate
    // singleton leaves a placeholder with a null pDesc so that the index
    // arithmetic below keeps working.
    std::vector<D3D12_STATE_SUBOBJECT> m_subs;
    // (subobject index, index of the subobject it associates with). Applied in
    // Desc(), once m_subs has stopped moving.
    std::vector<std::pair<size_t, size_t>> m_assocFixups;
    // Placeholders removed. This is what D3D12 actually sees, and what
    // association pointers must point into. Rebuilt by each Desc() call.
    std::vector<D3D12_STATE_SUBOBJECT> m_final;
};

// Attaches a StateObjectStore to an ID3D12StateObject through
// SetPrivateDataInterface, so the cache entry lives exactly as long as the
// object it describes. Keying a side table on the raw pointer would leak, and
// worse, a freed object's address can be reused and pick up a stale entry.
HRESULT StateObjectCacheAttach(ID3D12StateObject* so, StateObjectStore* store);

// Returns the store attached above, or null. Caller does not own it.
StateObjectStore* StateObjectCacheGet(ID3D12StateObject* so);
