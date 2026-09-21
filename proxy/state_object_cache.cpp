// Phase 4 - StateObjectStore implementation. See state_object_cache.h for why.

#include "state_object_cache.h"
#include "proxy_log.h"

#include <windows.h>
#include <new>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
LPCWSTR StateObjectStore::Intern(LPCWSTR s) {
    if (!s) return nullptr;
    m_strings.emplace_back(s);
    return m_strings.back().c_str();
}

const D3D12_EXPORT_DESC* StateObjectStore::CopyExports(const D3D12_EXPORT_DESC* p, UINT n) {
    if (!p || !n) return nullptr;
    m_exportArrays.emplace_back();
    auto& v = m_exportArrays.back();
    v.resize(n);
    for (UINT i = 0; i < n; ++i) {
        v[i].Name           = Intern(p[i].Name);
        v[i].ExportToRename = Intern(p[i].ExportToRename);
        v[i].Flags          = p[i].Flags;
    }
    return v.data();
}

LPCWSTR* StateObjectStore::CopyNames(const LPCWSTR* p, UINT n) {
    if (!p || !n) return nullptr;
    m_nameArrays.emplace_back();
    auto& v = m_nameArrays.back();
    v.resize(n);
    for (UINT i = 0; i < n; ++i) v[i] = Intern(p[i]);
    return v.data();
}

bool StateObjectStore::Has(D3D12_STATE_SUBOBJECT_TYPE t) const {
    for (const auto& s : m_subs) if (s.Type == t) return true;
    return false;
}

// Subobject types that may appear at most once in a state object. An addition
// repeats them, so merging has to drop the repeat.
static bool IsSingleton(D3D12_STATE_SUBOBJECT_TYPE t) {
    switch (t) {
    case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
    case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
    case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:
        return true;
    default:
        return false;
    }
}

static const char* TypeName(D3D12_STATE_SUBOBJECT_TYPE t) {
    switch (t) {
    case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:                return "STATE_OBJECT_CONFIG";
    case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:              return "GLOBAL_ROOT_SIGNATURE";
    case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:               return "LOCAL_ROOT_SIGNATURE";
    case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:                          return "NODE_MASK";
    case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:                       return "DXIL_LIBRARY";
    case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:                return "EXISTING_COLLECTION";
    case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:   return "SUBOBJECT_TO_EXPORTS_ASSOCIATION";
    case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION: return "DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION";
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:           return "RAYTRACING_SHADER_CONFIG";
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:         return "RAYTRACING_PIPELINE_CONFIG";
    case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:                          return "HIT_GROUP";
    case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1:        return "RAYTRACING_PIPELINE_CONFIG1";
    default:                                                            return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
bool StateObjectStore::Append(const D3D12_STATE_OBJECT_DESC& desc,
                              bool dropDuplicateSingletons,
                              std::string& why) {
    const size_t base = m_subs.size();

    for (UINT i = 0; i < desc.NumSubobjects; ++i) {
        const D3D12_STATE_SUBOBJECT& src = desc.pSubobjects[i];
        if (!src.pDesc) { why = "subobject with null pDesc"; return false; }

        if (dropDuplicateSingletons && IsSingleton(src.Type) && Has(src.Type)) {
            // The addition is required to repeat these; the grown object needs
            // exactly one. We keep the base's. If an app ever needs to RAISE a
            // config (a bigger payload, say) this is where that would show up,
            // so make it visible rather than silent.
            ProxyLog("[dxr11-proxy]   merge: dropping duplicate %s from addition\n",
                     TypeName(src.Type));
            // A placeholder keeps index arithmetic aligned for association
            // fixups that refer to it by position.
            m_subs.push_back(D3D12_STATE_SUBOBJECT{ src.Type, nullptr });
            continue;
        }

        D3D12_STATE_SUBOBJECT out{};
        out.Type = src.Type;

        switch (src.Type) {
        case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG: {
            m_cfg.push_back(*static_cast<const D3D12_STATE_OBJECT_CONFIG*>(src.pDesc));
            out.pDesc = &m_cfg.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE: {
            auto in = *static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(src.pDesc);
            if (in.pGlobalRootSignature) m_keepAlive.emplace_back(in.pGlobalRootSignature);
            m_globalRs.push_back(in);
            out.pDesc = &m_globalRs.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
            auto in = *static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(src.pDesc);
            if (in.pLocalRootSignature) m_keepAlive.emplace_back(in.pLocalRootSignature);
            m_localRs.push_back(in);
            out.pDesc = &m_localRs.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK: {
            m_nodeMask.push_back(*static_cast<const D3D12_NODE_MASK*>(src.pDesc));
            out.pDesc = &m_nodeMask.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
            auto in = *static_cast<const D3D12_DXIL_LIBRARY_DESC*>(src.pDesc);
            // The bytecode itself must be copied: nothing says the app keeps it
            // alive past CreateStateObject, and we rebuild much later.
            if (in.DXILLibrary.pShaderBytecode && in.DXILLibrary.BytecodeLength) {
                const auto* p = static_cast<const uint8_t*>(in.DXILLibrary.pShaderBytecode);
                m_blobs.emplace_back(p, p + in.DXILLibrary.BytecodeLength);
                in.DXILLibrary.pShaderBytecode = m_blobs.back().data();
            }
            in.pExports = const_cast<D3D12_EXPORT_DESC*>(CopyExports(in.pExports, in.NumExports));
            m_lib.push_back(in);
            out.pDesc = &m_lib.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
            auto in = *static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(src.pDesc);
            if (in.pExistingCollection) m_keepAlive.emplace_back(in.pExistingCollection);
            in.pExports = const_cast<D3D12_EXPORT_DESC*>(CopyExports(in.pExports, in.NumExports));
            m_collection.push_back(in);
            out.pDesc = &m_collection.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
            auto in = *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(src.pDesc);
            // This is the one that cannot simply be copied: pSubobjectToAssociate
            // points into the CALLER's subobject array. Record which index it
            // refers to and repoint it in Desc(), once our own array is stable.
            if (in.pSubobjectToAssociate) {
                ptrdiff_t idx = in.pSubobjectToAssociate - desc.pSubobjects;
                if (idx < 0 || (UINT)idx >= desc.NumSubobjects) {
                    why = "SUBOBJECT_TO_EXPORTS_ASSOCIATION points outside the subobject array";
                    return false;
                }
                m_assocFixups.emplace_back(m_subs.size(), base + (size_t)idx);
            }
            in.pExports = CopyNames(in.pExports, in.NumExports);
            m_assoc.push_back(in);
            out.pDesc = &m_assoc.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
            auto in = *static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(src.pDesc);
            in.SubobjectToAssociate = Intern(in.SubobjectToAssociate);
            in.pExports = CopyNames(in.pExports, in.NumExports);
            m_dxilAssoc.push_back(in);
            out.pDesc = &m_dxilAssoc.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: {
            m_shaderCfg.push_back(*static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(src.pDesc));
            out.pDesc = &m_shaderCfg.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: {
            m_pipeCfg.push_back(*static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(src.pDesc));
            out.pDesc = &m_pipeCfg.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1: {
            m_pipeCfg1.push_back(*static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1*>(src.pDesc));
            out.pDesc = &m_pipeCfg1.back();
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
            auto in = *static_cast<const D3D12_HIT_GROUP_DESC*>(src.pDesc);
            in.HitGroupExport             = Intern(in.HitGroupExport);
            in.AnyHitShaderImport         = Intern(in.AnyHitShaderImport);
            in.ClosestHitShaderImport     = Intern(in.ClosestHitShaderImport);
            in.IntersectionShaderImport   = Intern(in.IntersectionShaderImport);
            m_hitGroup.push_back(in);
            out.pDesc = &m_hitGroup.back();
            break;
        }
        default:
            // Fail loudly. Dropping an unknown subobject would build a state
            // object that differs from what the app asked for.
            why = std::string("unsupported subobject type ") +
                  std::to_string((int)src.Type) + " (" + TypeName(src.Type) + ")";
            return false;
        }

        m_subs.push_back(out);
    }
    return true;
}

bool StateObjectStore::HasAdditionsFlag() const {
    for (const auto& c : m_cfg)
        if (c.Flags & D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS) return true;
    return false;
}

void StateObjectStore::StripAdditionsFlag() {
    for (auto& c : m_cfg)
        c.Flags = (D3D12_STATE_OBJECT_FLAGS)(c.Flags &
                  ~D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS);
}

D3D12_STATE_OBJECT_DESC StateObjectStore::Desc(D3D12_STATE_OBJECT_TYPE type) {
    // Compact first: placeholders left by singleton de-duplication must not
    // reach D3D12. Keep a map from working index to final index, because
    // association pointers have to point into the FINAL array, which is the one
    // D3D12 sees.
    const size_t kDropped = (size_t)-1;
    std::vector<size_t> map(m_subs.size(), kDropped);
    m_final.clear();
    m_final.reserve(m_subs.size());
    for (size_t i = 0; i < m_subs.size(); ++i) {
        if (!m_subs[i].pDesc) continue;          // placeholder
        map[i] = m_final.size();
        m_final.push_back(m_subs[i]);
    }

    for (const auto& fx : m_assocFixups) {
        if (fx.first >= map.size() || fx.second >= map.size()) continue;
        const size_t assocIdx = map[fx.first];
        if (assocIdx == kDropped) continue;      // the association itself went away
        size_t targetIdx = map[fx.second];
        if (targetIdx == kDropped) {
            // The association pointed at a duplicate singleton we dropped during
            // a merge. Retarget it at the surviving subobject of the same type,
            // which is the one the grown object actually uses.
            const D3D12_STATE_SUBOBJECT_TYPE want = m_subs[fx.second].Type;
            for (size_t j = 0; j < m_final.size(); ++j) {
                if (m_final[j].Type == want) { targetIdx = j; break; }
            }
            if (targetIdx == kDropped) {
                ProxyLog("[dxr11-proxy]   merge: association target %s vanished\n",
                         TypeName(want));
                continue;
            }
        }
        auto* a = const_cast<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(
            static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(m_final[assocIdx].pDesc));
        if (a) a->pSubobjectToAssociate = &m_final[targetIdx];
    }

    D3D12_STATE_OBJECT_DESC d{};
    d.Type = type;
    d.NumSubobjects = (UINT)m_final.size();
    d.pSubobjects = m_final.data();
    return d;
}

// --- per-object cache -------------------------------------------------------
//
// {b0d0f2a1-...} is a private GUID for our own use. SetPrivateDataInterface
// makes the state object own a reference to the holder, so the store is freed
// exactly when the object it describes is.
static const GUID kStoreGuid =
    { 0xb0d0f2a1, 0x4c7e, 0x4f2a, { 0x9a, 0x11, 0x3d, 0x52, 0x77, 0x0b, 0xc4, 0x31 } };

namespace {
class StoreHolder : public IUnknown {
public:
    explicit StoreHolder(StateObjectStore* s) : m_store(s), m_refs(1) {}
    StateObjectStore* Store() const { return m_store; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) { AddRef(); *ppv = this; return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&m_refs);
        if (n == 0) { delete m_store; delete this; }
        return (ULONG)n;
    }
private:
    ~StoreHolder() = default;
    StateObjectStore* m_store;
    LONG m_refs;
};
} // namespace

HRESULT StateObjectCacheAttach(ID3D12StateObject* so, StateObjectStore* store) {
    if (!so || !store) return E_INVALIDARG;
    auto* holder = new (std::nothrow) StoreHolder(store);
    if (!holder) { delete store; return E_OUTOFMEMORY; }
    HRESULT hr = so->SetPrivateDataInterface(kStoreGuid, holder);
    holder->Release();   // the object holds its own reference now
    return hr;
}

StateObjectStore* StateObjectCacheGet(ID3D12StateObject* so) {
    if (!so) return nullptr;
    IUnknown* unk = nullptr;
    UINT size = sizeof(unk);
    if (FAILED(so->GetPrivateData(kStoreGuid, &size, &unk)) || !unk) return nullptr;
    auto* holder = static_cast<StoreHolder*>(unk);
    StateObjectStore* s = holder->Store();
    unk->Release();      // GetPrivateData on an interface returns an AddRef'd pointer
    return s;
}
