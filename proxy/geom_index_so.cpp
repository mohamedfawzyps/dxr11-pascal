// GeometryIndex() in an application's own DXR 1.0 hit shaders. See the header.
#include "geom_index_so.h"

#include "as_tracker.h"
#include "dxil_scan.h"
#include "geom_table_cs.h"
#include "gpu_hold.h"
#include "proxy_log.h"
#include "rewriter/dxc_host.h"
#include "rewriter/geom_index.h"
#include "rewriter/ll_model.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <regex>
#include <set>

using Microsoft::WRL::ComPtr;

namespace gidx {
namespace {

// {6C1B5E2A-9D3F-4E61-8A7C-2F0D11A5B9E3}
const GUID kBlobGuid = { 0x6c1b5e2a, 0x9d3f, 0x4e61, { 0x8a, 0x7c, 0x2f, 0x0d, 0x11, 0xa5, 0xb9, 0xe3 } };
// {3F8E7B10-51C2-4B9A-9E44-7D2A6C0E8F15}
const GUID kInfoGuid = { 0x3f8e7b10, 0x51c2, 0x4b9a, { 0x9e, 0x44, 0x7d, 0x2a, 0x6c, 0x0e, 0x8f, 0x15 } };

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const std::wstring& s) {
    std::string r;
    for (wchar_t c : s) r.push_back(c < 128 ? (char)c : '?');
    return r;
}

// The TraceRay arguments of one disassembled library.
void TraceArgs(const std::string& text, Info* info) {
    // call void @dx.op.traceRay.T(i32 157, handle, flags, mask, R, M, miss, ...)
    static const std::regex kTrace(
        R"RX(@dx\.op\.traceRay\.[^(]+\(i32 157, [^,]+, [^,]+, [^,]+, i32 ([^,]+), i32 ([^,]+),)RX");
    static const std::regex kNum(R"RX(^-?\d+$)RX");
    for (std::sregex_iterator it(text.begin(), text.end(), kTrace), end; it != end; ++it) {
        const std::string r = (*it)[1].str(), m = (*it)[2].str();
        if (!std::regex_match(r, kNum) || !std::regex_match(m, kNum)) {
            info->dynamicTraceArgs = true;
            continue;
        }
        const std::pair<UINT, UINT> p((UINT)std::stol(r) & 15u, (UINT)std::stol(m) & 15u);
        if (std::find(info->traceArgs.begin(), info->traceArgs.end(), p) == info->traceArgs.end())
            info->traceArgs.push_back(p);
    }
}

UINT Align(UINT v, UINT a) { return (v + a - 1) / a * a; }

// Where the appended constant lands in the local arguments, which follow the
// shader identifier: parameters in order, root constants 4-byte aligned,
// descriptors and tables 8-byte aligned.
UINT ArgsEnd(const D3D12_ROOT_SIGNATURE_DESC1& d) {
    UINT off = 0;
    for (UINT i = 0; i < d.NumParameters; ++i) {
        const auto& p = d.pParameters[i];
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
            off = Align(off, 4) + 4 * p.Constants.Num32BitValues;
        else
            off = Align(off, 8) + 8;
    }
    return off;
}

// An extended copy of a local root signature, or one holding only the
// constant when `rs` is null. `offset` receives the constant's offset in the
// record, identifier included.
bool Extend(ID3D12Device* dev, ID3D12RootSignature* rs, ComPtr<ID3D12RootSignature>* out,
            UINT* offset, std::string* why) {
    D3D12_ROOT_PARAMETER1 extra{};
    extra.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    extra.Constants.ShaderRegister = 0;
    extra.Constants.RegisterSpace = rq::kGeomIndexSpace;
    extra.Constants.Num32BitValues = 1;
    extra.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v{};
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    std::vector<D3D12_ROOT_PARAMETER1> params;
    ComPtr<ID3D12VersionedRootSignatureDeserializer> des;
    if (rs) {
        UINT size = 0;
        if (FAILED(rs->GetPrivateData(kBlobGuid, &size, nullptr)) || !size) {
            *why = "a local root signature the shim did not see being created";
            return false;
        }
        std::vector<uint8_t> blob(size);
        rs->GetPrivateData(kBlobGuid, &size, blob.data());
        if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob.data(), blob.size(),
                                                                 IID_PPV_ARGS(&des)))) {
            *why = "could not read back a local root signature";
            return false;
        }
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* got = nullptr;
        if (FAILED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &got)) || !got) {
            *why = "could not convert a local root signature to version 1.1";
            return false;
        }
        v.Desc_1_1 = got->Desc_1_1;
        params.assign(got->Desc_1_1.pParameters,
                      got->Desc_1_1.pParameters + got->Desc_1_1.NumParameters);
    } else {
        v.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    }
    D3D12_ROOT_SIGNATURE_DESC1 before = v.Desc_1_1;
    before.NumParameters = (UINT)params.size();
    before.pParameters = params.data();
    *offset = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + Align(ArgsEnd(before), 4);
    params.push_back(extra);
    v.Desc_1_1.NumParameters = (UINT)params.size();
    v.Desc_1_1.pParameters = params.data();
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeVersionedRootSignature(&v, &blob, &err))) {
        *why = std::string("could not serialize an extended local root signature: ") +
               (err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                        IID_PPV_ARGS(out->GetAddressOf())))) {
        *why = "could not create an extended local root signature";
        return false;
    }
    return true;
}

class InfoHolder : public IUnknown {
public:
    explicit InfoHolder(std::shared_ptr<Info> i) : m_info(std::move(i)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (riid == __uuidof(IUnknown)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&m_refs);
        if (!r) delete this;
        return r;
    }
    const std::shared_ptr<Info>& Get() const { return m_info; }
private:
    LONG m_refs = 1;
    std::shared_ptr<Info> m_info;
};

}  // namespace

void NoteRootSignature(ID3D12RootSignature* rs, const void* blob, size_t size) {
    if (rs && blob && size) rs->SetPrivateData(kBlobGuid, (UINT)size, blob);
}

Outcome Transform(ID3D12Device* dev, const D3D12_STATE_OBJECT_DESC& in,
                  Transformed* out, std::string* why) {
    const UINT n = in.NumSubobjects;
    const D3D12_STATE_SUBOBJECT* s = in.pSubobjects;
    if (!n || !s) return Outcome::kNothing;

    // 1. Which libraries read GeometryIndex(). Only those carrying the
    //    Tier 1.1 feature bit can, so everything else costs a flag test.
    struct Lib { UINT index; std::string text; std::vector<std::string> fns; std::vector<uint8_t> code; };
    std::vector<Lib> rewritten;
    std::set<std::wstring> readers;   // export names of functions that read it
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !s[i].pDesc) continue;
        const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc);
        if (!Dxr11ContainerUsesRayQuery(ld->DXILLibrary.pShaderBytecode,
                                        ld->DXILLibrary.BytecodeLength))
            continue;
        std::string text, err;
        if (!dxch::Disassemble(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength,
                               &text, &err))
            continue;
        if (text.find("@dx.op.geometryIndex.i32(") == std::string::npos) continue;
        Lib L;
        L.index = i;
        std::string lowered;
        if (!rq::LowerGeometryIndex(llm::Normalize(text), &lowered, &L.fns, why))
            return Outcome::kRefused;
        if (!dxch::AssembleAndSign(lowered, &L.code, &err)) {
            *why = "the rewritten library did not assemble: " + err;
            return Outcome::kRefused;
        }
        // Names as exported: a D3D12_EXPORT_DESC can rename a function.
        for (const auto& f : L.fns) {
            const std::wstring w = Wide(f);
            if (!ld->NumExports) { readers.insert(w); continue; }
            for (UINT e = 0; e < ld->NumExports; ++e) {
                const auto& x = ld->pExports[e];
                const std::wstring from = x.ExportToRename ? x.ExportToRename : x.Name;
                if (from == w) readers.insert(x.Name);
            }
        }
        L.text = std::move(text);
        rewritten.push_back(std::move(L));
    }
    if (rewritten.empty()) return Outcome::kNothing;

    // 2. The hit groups those functions serve, closed over shared shaders so
    //    that every shader of an extended group has the extended signature.
    struct HG { UINT index; std::wstring name; std::vector<std::wstring> imports; };
    std::vector<HG> groups;
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP || !s[i].pDesc) continue;
        const auto* h = static_cast<const D3D12_HIT_GROUP_DESC*>(s[i].pDesc);
        HG g{ i, h->HitGroupExport ? h->HitGroupExport : L"", {} };
        for (LPCWSTR imp : { h->AnyHitShaderImport, h->ClosestHitShaderImport,
                             h->IntersectionShaderImport })
            if (imp && *imp) g.imports.push_back(imp);
        groups.push_back(g);
    }
    std::set<std::wstring> touched = readers;   // shaders whose groups extend
    std::vector<bool> ext(groups.size(), false);
    for (bool grew = true; grew;) {
        grew = false;
        for (size_t k = 0; k < groups.size(); ++k) {
            if (ext[k]) continue;
            for (const auto& imp : groups[k].imports)
                if (touched.count(imp)) { ext[k] = true; break; }
            if (!ext[k]) continue;
            grew = true;
            for (const auto& imp : groups[k].imports) touched.insert(imp);
        }
    }
    std::set<std::wstring> affected;   // every export whose signature changes
    for (size_t k = 0; k < groups.size(); ++k)
        if (ext[k]) {
            affected.insert(groups[k].name);
            for (const auto& imp : groups[k].imports) affected.insert(imp);
        }
    // A reader no hit group here uses is still an export the driver compiles,
    // and it now reads a constant, so it needs the extended signature too.
    // Nothing can run it from this object, so no record carries its value.
    std::vector<std::wstring> loose;
    for (const auto& r : readers)
        if (!affected.count(r)) { loose.push_back(r); affected.insert(r); }

    // 3. Which local root signature each extended group has now.
    std::map<std::wstring, UINT> explicitLrs;   // export -> subobject index
    std::vector<UINT> defaults;                 // default local root signatures
    std::vector<int> refs(n, 0);
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION && s[i].pDesc) {
            const auto* a = static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s[i].pDesc);
            for (UINT e = 0; e < a->NumExports; ++e)
                if (affected.count(a->pExports[e])) {
                    *why = "an extended hit group has a subobject associated from inside a "
                           "library, which is not handled yet";
                    return Outcome::kRefused;
                }
        }
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION || !s[i].pDesc)
            continue;
        const auto* a = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s[i].pDesc);
        const ptrdiff_t t = a->pSubobjectToAssociate - s;
        if (t < 0 || t >= (ptrdiff_t)n) {
            *why = "an association points outside the state object description";
            return Outcome::kRefused;
        }
        ++refs[t];
        if (s[t].Type != D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE) continue;
        if (!a->NumExports) defaults.push_back((UINT)t);
        for (UINT e = 0; e < a->NumExports; ++e) explicitLrs[a->pExports[e]] = (UINT)t;
    }
    for (UINT i = 0; i < n; ++i)
        if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE && !refs[i])
            defaults.push_back(i);
    std::sort(defaults.begin(), defaults.end());
    defaults.erase(std::unique(defaults.begin(), defaults.end()), defaults.end());

    auto lrsOf = [&](const HG& g, int* idx) -> bool {
        *idx = -1;
        auto it = explicitLrs.find(g.name);
        if (it != explicitLrs.end()) { *idx = (int)it->second; return true; }
        for (const auto& imp : g.imports) {
            auto jt = explicitLrs.find(imp);
            if (jt == explicitLrs.end()) continue;
            if (*idx >= 0 && *idx != (int)jt->second) return false;
            *idx = (int)jt->second;
        }
        if (*idx >= 0) return true;
        if (defaults.size() > 1) return false;
        if (defaults.size() == 1) *idx = (int)defaults[0];
        return true;
    };

    auto info = std::make_shared<Info>();
    // One extended signature per original (or per "none", keyed -1).
    std::map<int, std::pair<ComPtr<ID3D12RootSignature>, UINT>> extended;
    std::map<int, std::vector<std::wstring>> assocNames;
    for (size_t k = 0; k < groups.size(); ++k) {
        if (!ext[k]) continue;
        int idx;
        if (!lrsOf(groups[k], &idx)) {
            *why = "cannot tell which local root signature hit group " + Narrow(groups[k].name) +
                   " has";
            return Outcome::kRefused;
        }
        if (!extended.count(idx)) {
            ID3D12RootSignature* orig = idx >= 0
                ? static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(s[idx].pDesc)->pLocalRootSignature
                : nullptr;
            ComPtr<ID3D12RootSignature> rs;
            UINT offset = 0;
            if (!Extend(dev, orig, &rs, &offset, why)) return Outcome::kRefused;
            extended[idx] = { rs, offset };
        }
        Group g;
        g.name = groups[k].name;
        g.offset = extended[idx].second;
        info->groups.push_back(g);
        info->recordBytes = (std::max)(info->recordBytes, g.offset + 4);
        auto& names = assocNames[idx];
        names.push_back(groups[k].name);
        for (const auto& imp : groups[k].imports)
            if (std::find(names.begin(), names.end(), imp) == names.end()) names.push_back(imp);
    }
    for (const auto& r : loose) {
        int idx = -1;
        auto it = explicitLrs.find(r);
        if (it != explicitLrs.end()) idx = (int)it->second;
        else if (defaults.size() > 1) {
            *why = "cannot tell which local root signature " + Narrow(r) + " has";
            return Outcome::kRefused;
        } else if (defaults.size() == 1) idx = (int)defaults[0];
        if (!extended.count(idx)) {
            ID3D12RootSignature* orig = idx >= 0
                ? static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(s[idx].pDesc)->pLocalRootSignature
                : nullptr;
            ComPtr<ID3D12RootSignature> rs;
            UINT offset = 0;
            if (!Extend(dev, orig, &rs, &offset, why)) return Outcome::kRefused;
            extended[idx] = { rs, offset };
        }
        assocNames[idx].push_back(r);
    }
    if (info->groups.empty() && loose.empty()) return Outcome::kNothing;

    // 4. TraceRay arguments, from every library in the object.
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !s[i].pDesc) continue;
        const Lib* r = nullptr;
        for (const auto& L : rewritten) if (L.index == i) r = &L;
        if (r) { TraceArgs(r->text, info.get()); continue; }
        const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc);
        std::string text, err;
        if (dxch::Disassemble(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength,
                              &text, &err))
            TraceArgs(text, info.get());
    }
    for (UINT i = 0; i < n; ++i)
        if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION)
            info->dynamicTraceArgs = true;   // its TraceRay calls are not visible here

    // 5. The new subobject array. Associations to a local root signature lose
    //    the extended exports; one left with none is dropped rather than
    //    becoming a DEFAULT association, and a signature left with no
    //    association is dropped rather than becoming a default one.
    std::vector<int> keep(n, 1);
    std::vector<std::vector<LPCWSTR>> filtered(n);
    std::vector<int> refsAfter(n, 0);
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION || !s[i].pDesc)
            continue;
        const auto* a = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s[i].pDesc);
        const UINT t = (UINT)(a->pSubobjectToAssociate - s);
        for (UINT e = 0; e < a->NumExports; ++e)
            if (s[t].Type != D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE ||
                !affected.count(a->pExports[e]))
                filtered[i].push_back(a->pExports[e]);
        if (a->NumExports && filtered[i].empty()) keep[i] = 0;
        else ++refsAfter[t];
    }
    for (UINT i = 0; i < n; ++i)
        if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE && refs[i] && !refsAfter[i])
            keep[i] = 0;
    std::vector<int> remap(n, -1);
    UINT m = 0;
    for (UINT i = 0; i < n; ++i) if (keep[i]) remap[i] = (int)m++;
    out->subs.clear();
    out->subs.reserve(m + 2 * extended.size());
    for (UINT i = 0; i < n; ++i) {
        if (!keep[i]) continue;
        D3D12_STATE_SUBOBJECT so = s[i];
        if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
            for (const auto& L : rewritten)
                if (L.index == i) {
                    out->code.push_back(L.code);
                    D3D12_DXIL_LIBRARY_DESC ld = *static_cast<const D3D12_DXIL_LIBRARY_DESC*>(so.pDesc);
                    ld.DXILLibrary.pShaderBytecode = out->code.back().data();
                    ld.DXILLibrary.BytecodeLength = out->code.back().size();
                    out->libs.push_back(ld);
                    so.pDesc = &out->libs.back();
                }
        } else if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) {
            const auto* a = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(so.pDesc);
            out->exportLists.push_back(filtered[i]);
            D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION na{};
            na.NumExports = (UINT)out->exportLists.back().size();
            na.pExports = na.NumExports ? out->exportLists.back().data() : nullptr;
            // Placeholder; fixed below once the array is complete.
            na.pSubobjectToAssociate = reinterpret_cast<const D3D12_STATE_SUBOBJECT*>(
                (uintptr_t)remap[a->pSubobjectToAssociate - s]);
            out->assoc.push_back(na);
            so.pDesc = &out->assoc.back();
        }
        out->subs.push_back(so);
    }
    for (auto& kv : extended) {
        out->sigs.push_back(kv.second.first);
        D3D12_LOCAL_ROOT_SIGNATURE l{ kv.second.first.Get() };
        out->lrs.push_back(l);
        const size_t at = out->subs.size();
        out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &out->lrs.back() });
        std::vector<LPCWSTR> list;
        for (const auto& w : assocNames[kv.first]) {
            out->names.push_back(w);
            list.push_back(out->names.back().c_str());
        }
        out->exportLists.push_back(list);
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION na{};
        na.NumExports = (UINT)list.size();
        na.pExports = out->exportLists.back().data();
        na.pSubobjectToAssociate = reinterpret_cast<const D3D12_STATE_SUBOBJECT*>((uintptr_t)at);
        out->assoc.push_back(na);
        out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
                              &out->assoc.back() });
    }
    for (auto& a : out->assoc)
        a.pSubobjectToAssociate = &out->subs[(size_t)(uintptr_t)a.pSubobjectToAssociate];
    out->desc.Type = in.Type;
    out->desc.NumSubobjects = (UINT)out->subs.size();
    out->desc.pSubobjects = out->subs.data();
    out->info = info;

    std::string fns;
    for (const auto& w : readers) fns += (fns.empty() ? "" : ", ") + Narrow(w);
    std::string trace;
    for (const auto& p : info->traceArgs)
        trace += (trace.empty() ? "" : ", ") + std::string("(") + std::to_string(p.first) + ", " +
                 std::to_string(p.second) + ")";
    out->summary = "GeometryIndex() read in " + fns + "; " + std::to_string(info->groups.size()) +
                   " hit group(s) extended, " + std::to_string(extended.size()) +
                   " local root signature(s), record " + std::to_string(info->recordBytes) +
                   " bytes, TraceRay (R, M) " + (trace.empty() ? "none" : trace) +
                   (info->dynamicTraceArgs ? " plus some not known here" : "");
    return Outcome::kTransformed;
}

void Attach(ID3D12StateObject* so, const std::shared_ptr<Info>& info) {
    if (!so || !info) return;
    ComPtr<ID3D12StateObjectProperties> props;
    if (SUCCEEDED(so->QueryInterface(IID_PPV_ARGS(&props))))
        for (auto& g : info->groups)
            if (void* id = props->GetShaderIdentifier(g.name.c_str()))
                std::memcpy(g.id, id, sizeof(g.id));
    auto* h = new InfoHolder(info);
    so->SetPrivateDataInterface(kInfoGuid, h);
    h->Release();
}

std::shared_ptr<Info> Get(ID3D12StateObject* so) {
    if (!so) return nullptr;
    IUnknown* unk = nullptr;
    UINT size = sizeof(unk);
    if (FAILED(so->GetPrivateData(kInfoGuid, &size, &unk)) || !unk) return nullptr;
    auto info = static_cast<InfoHolder*>(unk)->Get();
    unk->Release();
    return info;
}

// --- the shim's copy of the table ---------------------------------------------

namespace {

struct Pipe {
    ID3D12Device* device = nullptr;
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
};
struct Slot {
    ID3D12Device* device;
    D3D12_HEAP_TYPE type;
    UINT64 size;
    ComPtr<ID3D12Resource> res;
};
std::mutex g_lock;
Pipe g_pipe;
std::vector<Slot> g_pool;

ComPtr<ID3D12Resource> Acquire(ID3D12Device* dev, D3D12_HEAP_TYPE type, UINT64 size,
                               const void* owner) {
    // Caller holds g_lock, so no other thread can take the same free buffer
    // between the Busy test and the Use that marks it taken.
    for (auto& sl : g_pool)
        if (sl.device == dev && sl.type == type && sl.size >= size && !gpuhold::Busy(sl.res.Get())) {
            gpuhold::Use(sl.res.Get(), owner);
            return sl.res;
        }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = type == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                               : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                           : D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    // Trim: idle buffers beyond a few dozen go.
    if (g_pool.size() >= 48)
        for (auto it = g_pool.begin(); it != g_pool.end();)
            if (!gpuhold::Busy(it->res.Get())) it = g_pool.erase(it);
            else ++it;
    g_pool.push_back({ dev, type, size, r });
    gpuhold::Use(r.Get(), owner);
    return r;
}

}  // namespace

bool RecordTable(ID3D12GraphicsCommandList4* cl, ID3D12Device* dev, const Info& info,
                 const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE& app,
                 D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE* shim,
                 const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& boundSrvs,
                 const void* owner, std::string* why) {
    if (info.dynamicTraceArgs) {
        *why = "a TraceRay whose hit group arguments are not known at pipeline creation "
               "(computed at run time, or in a collection)";
        return false;
    }
    if (!app.StrideInBytes) {
        *why = "a hit group table with stride 0, one record for every geometry";
        return false;
    }
    const UINT records = (UINT)(app.SizeInBytes / app.StrideInBytes);
    if (!records) { *shim = app; return true; }
    bool read = false;
    const auto labels = astrack::GeometryLabels(info.traceArgs, records, boundSrvs, &read);
    if (!read) {
        *why = "no top-level structure has been read yet";
        return false;
    }
    for (UINT r = 0; r < records; ++r)
        if (labels[r] == -2) {
            *why = "hit group record " + std::to_string(r) + " is reached by two different "
                   "geometries, so it cannot answer GeometryIndex(); the shim's own table "
                   "layout for this case is not built yet";
            return false;
        }

    const UINT srcStride = (UINT)app.StrideInBytes;
    const UINT dstStride = Align((std::max)(srcStride, info.recordBytes),
                                 D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    const UINT groups = (UINT)info.groups.size();
    const UINT metaDwords = groups * 9 + records;

    std::lock_guard<std::mutex> g(g_lock);
    if (g_pipe.device != dev || !g_pipe.pso) {
        Pipe p;
        p.device = dev;
        if (FAILED(dev->CreateRootSignature(0, g_geomTableCS, sizeof(g_geomTableCS),
                                            IID_PPV_ARGS(&p.rs)))) {
            *why = "could not create the table copy root signature";
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = p.rs.Get();
        pd.CS.pShaderBytecode = g_geomTableCS;
        pd.CS.BytecodeLength = sizeof(g_geomTableCS);
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&p.pso)))) {
            *why = "could not create the table copy pipeline";
            return false;
        }
        g_pipe = p;
    }
    auto meta = Acquire(dev, D3D12_HEAP_TYPE_UPLOAD, (UINT64)metaDwords * 4, owner);
    auto dst = Acquire(dev, D3D12_HEAP_TYPE_DEFAULT, (UINT64)records * dstStride, owner);
    if (!meta || !dst) {
        *why = "could not create the table copy buffers";
        return false;
    }
    uint32_t* m = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(meta->Map(0, &none, reinterpret_cast<void**>(&m)))) {
        *why = "could not map the table copy metadata";
        return false;
    }
    for (UINT k = 0; k < groups; ++k) {
        std::memcpy(m + k * 9, info.groups[k].id, 32);
        m[k * 9 + 8] = info.groups[k].offset;
    }
    // DXR_TIER11_GI_POISON=1 writes 0 everywhere: the sensitivity check, which
    // must turn tier11\gitest.exe from MATCH to DIVERGE.
    static const bool poison = GetEnvironmentVariableA("DXR_TIER11_GI_POISON", nullptr, 0) != 0;
    for (UINT r = 0; r < records; ++r)
        m[groups * 9 + r] = (labels[r] >= 0 && !poison) ? (uint32_t)labels[r] : 0u;
    meta->Unmap(0, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = dst.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);
    cl->SetComputeRootSignature(g_pipe.rs.Get());
    cl->SetPipelineState(g_pipe.pso.Get());
    const UINT c[4] = { records, srcStride, dstStride, groups };
    cl->SetComputeRoot32BitConstants(0, 4, c, 0);
    cl->SetComputeRootShaderResourceView(1, app.StartAddress);
    cl->SetComputeRootShaderResourceView(2, meta->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(3, dst->GetGPUVirtualAddress());
    cl->Dispatch((records + 63) / 64, 1, 1);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);

    shim->StartAddress = dst->GetGPUVirtualAddress();
    shim->SizeInBytes = (UINT64)records * dstStride;
    shim->StrideInBytes = dstStride;
    return true;
}

}  // namespace gidx
