// GeometryIndex() in an application's own DXR 1.0 hit shaders. See the header.
#include "geom_index_so.h"

#include "as_tracker.h"
#include "dxil_scan.h"
#include "geom_table_cs.h"
#include "gpu_hold.h"
#include "proxy_log.h"
#include "res_tracker.h"
#include "scene_bind.h"
#include "shim_scene.h"
#include "shim_table_cs.h"
#include "state_object_cache.h"
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

// A global's name from where it starts at `at` ('@'): quoted or plain.
std::string GlobalAt(const std::string& l, size_t at) {
    if (at == std::string::npos) return {};
    if (at + 1 < l.size() && l[at + 1] == '"') {
        const size_t e = l.find('"', at + 2);
        return e == std::string::npos ? std::string() : l.substr(at, e + 1 - at);
    }
    size_t e = at + 1;
    while (e < l.size() && (isalnum((unsigned char)l[e]) || l[e] == '_' || l[e] == '.' || l[e] == '$'))
        ++e;
    return l.substr(at, e - at);
}

void AddHeapScene(Scenes* sc, const Scenes::Heap& h) {
    for (const auto& x : sc->heap)
        if (x.space == h.space && x.reg == h.reg && x.offset == h.offset) return;
    sc->heap.push_back(h);
}

void AddScene(Scenes* sc, const Scenes::Reg& r) {
    for (const auto& x : sc->regs)
        if (x.space == r.space && x.lower == r.lower && x.count == r.count) return;
    sc->regs.push_back(r);
}

// The scene register of every TraceRay in one disassembled library: its
// handle traced back through annotateHandle and createHandleForLib to the
// load of a resource global, maybe through a getelementptr into an array,
// and that global's record in !dx.resources. Shapes read off DXC at lib_6_5
// and lib_6_6. Anything else, a descriptor heap index above all, is unknown.
void SceneRegs(const std::string& text, Scenes* sc) {
    struct Rec { UINT space; UINT lower; int size; };
    std::map<std::string, Rec> recs, anyRecs;   // structures; every resource
    static const std::regex kAnyFields(R"RX(, !"[^"]*", i32 (-?\d+), i32 (-?\d+), i32 (-?\d+),)RX");
    static const std::regex kHeap(R"RX(\(i32 218, i32 (%[^,]+), i1 false,)RX");
    static const std::regex kExtract(R"RX(extractvalue %dx\.types\.CBufRet\.i32 (%[^,]+), (\d+)$)RX");
    static const std::regex kCbLoad(R"RX(@dx\.op\.cbufferLoadLegacy\.i32\(i32 59, %dx\.types\.Handle (%[^,]+), i32 (\d+)\))RX");
    static const std::regex kFields(R"RX(, !"[^"]*", i32 (-?\d+), i32 (-?\d+), i32 (-?\d+), i32 16,)RX");
    static const std::regex kTrace(R"RX(@dx\.op\.traceRay\.[^(]+\(i32 157, %[^ ]+ (%[^,]+),)RX");
    static const std::regex kOperand(R"RX(\(i32 \d+, %[^ ]+ (%[^,)]+))RX");
    static const std::regex kConstElem(R"RX(i32 (\d+)\), align)RX");
    static const std::regex kPtrOperand(R"RX(\* (%[^,]+), align)RX");
    static const std::regex kLastIndex(R"RX(, i32 ([^,]+)$)RX");
    std::vector<std::string> lines;
    for (size_t at = 0; at < text.size();) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(at, e - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        at = e + 1;
    }
    std::smatch m;
    for (const auto& l : lines)
        if (l.rfind("!", 0) == 0 && l.find('@') != std::string::npos && std::regex_search(l, m, kAnyFields)) {
            const std::string g = GlobalAt(l, l.find('@'));
            if (!g.empty())
                anyRecs[g] = { (UINT)std::stol(m[1].str()), (UINT)std::stol(m[2].str()),
                               (int)std::stol(m[3].str()) };
        }
    for (const auto& l : lines)
        if (l.rfind("!", 0) == 0 && l.find("RaytracingAccelerationStructure") != std::string::npos &&
            std::regex_search(l, m, kFields)) {
            const std::string g = GlobalAt(l, l.find('@'));
            if (!g.empty())
                recs[g] = { (UINT)std::stol(m[1].str()), (UINT)std::stol(m[2].str()),
                            (int)std::stol(m[3].str()) };
        }
    std::map<std::string, const std::string*> defs;
    for (const auto& l : lines) {
        if (l.rfind("define ", 0) == 0) { defs.clear(); continue; }
        const size_t eq = l.find(" = ");
        if (l.rfind("  %", 0) == 0 && eq != std::string::npos) defs[l.substr(2, eq - 2)] = &l;
        if (l.find("@dx.op.traceRay.") == std::string::npos || !std::regex_search(l, m, kTrace))
            continue;
        std::string v = m[1].str();
        bool done = false;
        for (int step = 0; step < 8 && !done; ++step) {
            auto it = defs.find(v);
            if (it == defs.end()) break;
            const std::string& d = *it->second;
            if (d.find("@dx.op.annotateHandle(") != std::string::npos ||
                d.find("@dx.op.createHandleForLib") != std::string::npos) {
                if (!std::regex_search(d, m, kOperand)) break;
                v = m[1].str();
                continue;
            }
            if (d.find("@dx.op.createHandleFromHeap(") != std::string::npos) {
                // ResourceDescriptorHeap[i], i a dword of a cbuffer at a
                // constant row: Unreal's bindless scene, all 64 dumped.
                if (!std::regex_search(d, m, kHeap)) break;
                auto ei = defs.find(m[1].str());
                if (ei == defs.end() || !std::regex_search(*ei->second, m, kExtract)) break;
                const UINT comp = (UINT)std::stoul(m[2].str());
                auto li = defs.find(m[1].str());
                if (li == defs.end() || !std::regex_search(*li->second, m, kCbLoad)) break;
                const UINT row = (UINT)std::stoul(m[2].str());
                std::string h = m[1].str(), g;
                for (int k = 0; k < 4; ++k) {
                    auto hi = defs.find(h);
                    if (hi == defs.end()) break;
                    const std::string& hd = *hi->second;
                    if (hd.find("@dx.op.annotateHandle(") != std::string::npos ||
                        hd.find("@dx.op.createHandleForLib") != std::string::npos) {
                        if (!std::regex_search(hd, m, kOperand)) break;
                        h = m[1].str();
                        continue;
                    }
                    if (hd.find("= load ") != std::string::npos && hd.find('@') != std::string::npos)
                        g = GlobalAt(hd, hd.find('@'));
                    break;
                }
                auto r = anyRecs.find(g);
                if (r == anyRecs.end()) break;
                Scenes::Heap sh;
                sh.space = r->second.space;
                sh.reg = r->second.lower;
                sh.offset = row * 16 + comp * 4;
                AddHeapScene(sc, sh);
                done = true;
                break;
            }
            if (d.find("= load ") == std::string::npos) break;
            // The global, and the element when it is an array.
            std::string g, elem;
            bool dynamic = false;
            if (d.find('@') != std::string::npos) {
                g = GlobalAt(d, d.find('@'));
                if (d.find("getelementptr") != std::string::npos && std::regex_search(d, m, kConstElem))
                    elem = m[1].str();
            } else if (std::regex_search(d, m, kPtrOperand)) {
                auto gi = defs.find(m[1].str());
                if (gi == defs.end() || gi->second->find("getelementptr") == std::string::npos) break;
                const std::string& gep = *gi->second;
                g = GlobalAt(gep, gep.find('@'));
                if (!std::regex_search(gep, m, kLastIndex)) break;
                elem = m[1].str();
                dynamic = elem.empty() || !isdigit((unsigned char)elem[0]);
            }
            auto r = recs.find(g);
            if (r == recs.end()) break;
            Scenes::Reg sr;
            sr.space = r->second.space;
            if (dynamic) {
                sr.lower = r->second.lower;
                sr.count = r->second.size > 0 ? (UINT)r->second.size : 0;
            } else {
                sr.lower = r->second.lower + (elem.empty() ? 0u : (UINT)std::stoul(elem));
            }
            AddScene(sc, sr);
            done = true;
        }
        if (!done) sc->unknown = true;
    }
}

// The TraceRay arguments of one disassembled library, and their scenes.
void TraceArgs(const std::string& text, Info* info) {
    SceneRegs(text, &info->scenes);
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

// The shader kinds a library defines, read from its RDAT function table, so a
// library that cannot call TraceRay is never disassembled. Layout checked on
// a DXC 1.10 library: RDAT {version, part count, part offsets}, each part
// {type, size, data}, the function table (type 4) {count, stride, records},
// the kind in word 4 of a record (7 raygen, 9 any-hit, 10 closest-hit,
// 11 miss, 12 callable). False when there is no readable RDAT.
// The same table gives each function's unmangled name, word 1, an offset into
// the string buffer part (type 1).
bool Functions(const void* container, size_t size,
               std::vector<std::pair<std::string, uint32_t>>* fns) {
    const uint8_t* b = static_cast<const uint8_t*>(container);
    auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, b + at, 4); return v; };
    if (!b || size < 32 || std::memcmp(b, "DXBC", 4) != 0) return false;
    const uint32_t parts = u32(28);
    for (uint32_t i = 0; i < parts && 32 + 4 * (size_t)i + 4 <= size; ++i) {
        const size_t o = u32(32 + 4 * (size_t)i);
        if (o + 8 > size || std::memcmp(b + o, "RDAT", 4) != 0) continue;
        const size_t base = o + 8, len = u32(o + 4);
        if (base + len > size || len < 8) return false;
        const uint32_t count = u32(base + 4);
        size_t strings = 0, stringsLen = 0, table = 0;
        for (uint32_t p = 0; p < count && 8 + 4 * (size_t)p + 4 <= len; ++p) {
            const size_t po = base + u32(base + 8 + 4 * (size_t)p);
            if (po + 8 > base + len) return false;
            if (u32(po) == 1) { strings = po + 8; stringsLen = u32(po + 4); }
            if (u32(po) == 4) table = po + 8;
        }
        if (!table) return false;
        const uint32_t recs = u32(table), stride = u32(table + 4);
        if (stride < 20 || table + 8 + (size_t)recs * stride > base + len) return false;
        for (uint32_t r = 0; r < recs; ++r) {
            const size_t rec = table + 8 + (size_t)r * stride;
            std::string name;
            const uint32_t at = u32(rec + 4);
            if (strings && at < stringsLen)
                for (size_t c = strings + at; c < strings + stringsLen && b[c]; ++c)
                    name.push_back((char)b[c]);
            fns->emplace_back(name, u32(rec + 16));
        }
        return true;
    }
    return false;
}

// The local root signatures and associations a library declares itself (HLSL
// LocalRootSignature and SubobjectToExportsAssociation), from its RDAT.
// Layout read off a DXC 1.10 library: the subobject table (part type 6)
// {count, stride 24, records}, a record {kind, name, two words}: kind 2, a
// local root signature, {offset, size} into the raw bytes part (type 5);
// kind 8, an association, {subobject name, exports} where exports is an
// offset in the index arrays part (type 2), counted in dwords, holding
// {count, string offsets...}. False when there is no readable RDAT.
struct LibSubobjects {
    std::map<std::wstring, std::vector<uint8_t>> lrs;                         // name -> blob
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> assoc;    // subobject -> exports
    // Every subobject, in order. `kind` is the DXIL subobject kind, whose
    // values are the D3D12_STATE_SUBOBJECT_TYPE ones (8, an association, is
    // DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION). `w` the record's two words:
    // flags, sizes, depth and flags; `bytes` a root signature; `str` a hit
    // group's any-hit, closest-hit and intersection names; for an
    // association, `str[0]` its subobject and `exports` its list.
    struct Sub {
        uint32_t kind = 0;
        std::wstring name;
        uint32_t w[2] = {};
        std::vector<uint8_t> bytes;
        std::wstring str[3];
        std::vector<std::wstring> exports;
    };
    std::vector<Sub> all;
};

bool LibrarySubobjects(const void* container, size_t size, LibSubobjects* out) {
    const uint8_t* b = static_cast<const uint8_t*>(container);
    auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, b + at, 4); return v; };
    if (!b || size < 32 || std::memcmp(b, "DXBC", 4) != 0) return false;
    const uint32_t parts = u32(28);
    for (uint32_t i = 0; i < parts && 32 + 4 * (size_t)i + 4 <= size; ++i) {
        const size_t o = u32(32 + 4 * (size_t)i);
        if (o + 8 > size || std::memcmp(b + o, "RDAT", 4) != 0) continue;
        const size_t base = o + 8, len = u32(o + 4);
        if (base + len > size || len < 8) return false;
        const uint32_t count = u32(base + 4);
        size_t at[7] = {}, sz[7] = {};
        for (uint32_t p = 0; p < count && 8 + 4 * (size_t)p + 4 <= len; ++p) {
            const size_t po = base + u32(base + 8 + 4 * (size_t)p);
            if (po + 8 > base + len) return false;
            const uint32_t t = u32(po);
            if (t < 7) {
                at[t] = po + 8;
                sz[t] = u32(po + 4);
                if (at[t] + sz[t] > base + len) return false;
            }
        }
        if (!at[6]) return true;   // no subobjects
        auto str = [&](uint32_t off) {
            std::wstring w;
            if (!at[1] || off >= sz[1]) return w;
            for (size_t c = at[1] + off; c < at[1] + sz[1] && b[c]; ++c) w.push_back((wchar_t)b[c]);
            return w;
        };
        const uint32_t recs = u32(at[6]), stride = u32(at[6] + 4);
        if (stride < 16 || at[6] + 8 + (size_t)recs * stride > at[6] + sz[6]) return false;
        if (stride < 24) return false;
        for (uint32_t r = 0; r < recs; ++r) {
            const size_t rec = at[6] + 8 + (size_t)r * stride;
            const uint32_t kind = u32(rec), w0 = u32(rec + 8), w1 = u32(rec + 12);
            LibSubobjects::Sub sub;
            sub.kind = kind;
            sub.name = str(u32(rec + 4));
            sub.w[0] = w0;
            sub.w[1] = w1;
            if (kind == 1 || kind == 2) {
                if (!at[5] || (size_t)w0 + w1 > sz[5]) return false;
                sub.bytes.assign(b + at[5] + w0, b + at[5] + w0 + w1);
            } else if (kind == 11) {
                sub.str[0] = str(w1);
                sub.str[1] = str(u32(rec + 16));
                sub.str[2] = str(u32(rec + 20));
            }
            if (kind == 2) {
                out->lrs[sub.name] = sub.bytes;
            } else if (kind == 8) {
                std::vector<std::wstring> ex;
                if (at[2]) {
                    const size_t ia = at[2] + 4 * (size_t)w1;
                    if (ia + 4 > at[2] + sz[2]) return false;
                    const uint32_t n = u32(ia);
                    if (ia + 4 + 4 * (size_t)n > at[2] + sz[2]) return false;
                    for (uint32_t k = 0; k < n; ++k) ex.push_back(str(u32(ia + 4 + 4 * (size_t)k)));
                }
                out->assoc.emplace_back(str(w0), ex);
                sub.str[0] = str(w0);
                sub.exports = ex;
            }
            out->all.push_back(std::move(sub));
        }
        return true;
    }
    return false;
}

// A library stores a local root signature as the bare RTS0 part, where
// CreateRootSignature wants the serialized container. Decoded here into a
// version 1.1 description and serialized again by D3D12 itself, so no
// container or checksum is built by hand. Layout (versions 1 and 2, the
// latter being 1.1): header {version, parameters, parameter offset, static
// samplers, sampler offset, flags}; a parameter {type, visibility, payload
// offset}; constants {register, space, count}; a descriptor {register, space
// [, flags at 1.1]}; a table {ranges, range offset}, a range {type, count,
// base register, space, [flags at 1.1,] offset}; a static sampler 13 dwords.
bool SerializeRts0(const std::vector<uint8_t>& raw, std::vector<uint8_t>* out, std::string* why) {
    const uint8_t* b = raw.data();
    const size_t n = raw.size();
    auto u32 = [&](size_t at, uint32_t* v) {
        if (at + 4 > n) return false;
        std::memcpy(v, b + at, 4);
        return true;
    };
    uint32_t ver = 0, np = 0, po = 0, ns = 0, so = 0, flags = 0;
    if (!u32(0, &ver) || !u32(4, &np) || !u32(8, &po) || !u32(12, &ns) || !u32(16, &so) ||
        !u32(20, &flags) || (ver != 1 && ver != 2)) {
        *why = "a library's local root signature is not a version 1.0 or 1.1 RTS0 part";
        return false;
    }
    const bool v11 = ver == 2;
    std::vector<D3D12_ROOT_PARAMETER1> params(np);
    std::deque<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges;
    for (uint32_t i = 0; i < np; ++i) {
        uint32_t type = 0, vis = 0, off = 0;
        if (!u32(po + 12 * (size_t)i, &type) || !u32(po + 12 * (size_t)i + 4, &vis) ||
            !u32(po + 12 * (size_t)i + 8, &off))
            return *why = "a library's local root signature is truncated", false;
        auto& p = params[i];
        p.ParameterType = (D3D12_ROOT_PARAMETER_TYPE)type;
        p.ShaderVisibility = (D3D12_SHADER_VISIBILITY)vis;
        uint32_t a = 0, c = 0, d = 0;
        if (type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
            if (!u32(off, &a) || !u32(off + 4, &c) || !u32(off + 8, &d))
                return *why = "a library's local root signature is truncated", false;
            p.Constants = { a, c, d };
        } else if (type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            uint32_t nr = 0, ro = 0;
            if (!u32(off, &nr) || !u32(off + 4, &ro))
                return *why = "a library's local root signature is truncated", false;
            const size_t rs = v11 ? 24 : 20;
            ranges.emplace_back(nr);
            for (uint32_t r = 0; r < nr; ++r) {
                uint32_t w[6] = {};
                for (int k = 0; k < (v11 ? 6 : 5); ++k)
                    if (!u32(ro + rs * r + 4 * (size_t)k, &w[k]))
                        return *why = "a library's local root signature is truncated", false;
                auto& g = ranges.back()[r];
                g.RangeType = (D3D12_DESCRIPTOR_RANGE_TYPE)w[0];
                g.NumDescriptors = w[1];
                g.BaseShaderRegister = w[2];
                g.RegisterSpace = w[3];
                g.Flags = v11 ? (D3D12_DESCRIPTOR_RANGE_FLAGS)w[4] : D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
                g.OffsetInDescriptorsFromTableStart = v11 ? w[5] : w[4];
            }
            p.DescriptorTable = { nr, ranges.back().data() };
        } else {
            if (!u32(off, &a) || !u32(off + 4, &c) || (v11 && !u32(off + 8, &d)))
                return *why = "a library's local root signature is truncated", false;
            p.Descriptor = { a, c, v11 ? (D3D12_ROOT_DESCRIPTOR_FLAGS)d : D3D12_ROOT_DESCRIPTOR_FLAG_NONE };
        }
    }
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(ns);
    for (uint32_t i = 0; i < ns; ++i) {
        if ((size_t)so + 52 * ((size_t)i + 1) > n)
            return *why = "a library's local root signature is truncated", false;
        std::memcpy(&samplers[i], b + so + 52 * (size_t)i, 52);
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC v{};
    v.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    v.Desc_1_1 = { np, np ? params.data() : nullptr, ns, ns ? samplers.data() : nullptr,
                   (D3D12_ROOT_SIGNATURE_FLAGS)flags };
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeVersionedRootSignature(&v, &blob, &err))) {
        *why = "could not serialize a library's local root signature";
        return false;
    }
    const uint8_t* bp = static_cast<const uint8_t*>(blob->GetBufferPointer());
    out->assign(bp, bp + blob->GetBufferSize());
    return true;
}

bool ShaderKinds(const void* container, size_t size, std::vector<uint32_t>* kinds) {
    std::vector<std::pair<std::string, uint32_t>> fns;
    if (!Functions(container, size, &fns)) return false;
    for (const auto& f : fns) kinds->push_back(f.second);
    return true;
}

// A library's functions under the names it exports them by: a
// D3D12_EXPORT_DESC list can export a subset, renamed.
std::vector<std::pair<std::wstring, uint32_t>> Exported(const D3D12_DXIL_LIBRARY_DESC* ld) {
    std::vector<std::pair<std::wstring, uint32_t>> out;
    std::vector<std::pair<std::string, uint32_t>> fns;
    if (!Functions(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength, &fns))
        return out;
    for (const auto& f : fns) {
        const std::wstring w = Wide(f.first);
        if (!ld->NumExports) { out.emplace_back(w, f.second); continue; }
        for (UINT e = 0; e < ld->NumExports; ++e) {
            const auto& x = ld->pExports[e];
            if ((x.ExportToRename ? x.ExportToRename : x.Name) == w) out.emplace_back(x.Name, f.second);
        }
    }
    return out;
}

// The pipeline config's recursion depth, 0 when the object has none.
UINT Recursion(const D3D12_STATE_OBJECT_DESC& in) {
    for (UINT i = 0; i < in.NumSubobjects; ++i) {
        const auto& so = in.pSubobjects[i];
        if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG && so.pDesc)
            return static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(so.pDesc)->MaxTraceRecursionDepth;
        if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1 && so.pDesc)
            return static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1*>(so.pDesc)->MaxTraceRecursionDepth;
    }
    return 0;
}

// The TraceRay arguments of one library, when it has a shader that can call
// TraceRay: a raygen, or with a recursion depth above 1 (or unknown) a
// closest-hit or miss. Unreal's is 1, so only its raygen collections pay a
// disassembly.
std::vector<std::pair<std::wstring, uint32_t>> Exported(const D3D12_DXIL_LIBRARY_DESC* ld);

void LibraryTraceArgs(const D3D12_DXIL_LIBRARY_DESC* ld, UINT recursion, Info* info) {
    // Only what the object EXPORTS can run: a collection may carry a whole
    // library and export one hit shader of it.
    std::vector<uint32_t> kinds;
    const bool known = ShaderKinds(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength,
                                   &kinds);
    bool can = !known;
    for (const auto& f : Exported(ld))
        if (f.second == 7 || ((f.second == 10 || f.second == 11) && recursion != 1)) can = true;
    if (!can) return;
    std::string text, err;
    if (dxch::Disassemble(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength, &text,
                          &err))
        TraceArgs(text, info);
    else
        info->dynamicTraceArgs = info->scenes.unknown = true;
}

// What the collections this object links already know: their TraceRay
// arguments, and their extended hit groups under the names this object gives
// them (an EXISTING_COLLECTION can export a subset, renamed).
void MergeCollections(const D3D12_STATE_OBJECT_DESC& in, Info* info) {
    for (UINT i = 0; i < in.NumSubobjects; ++i) {
        const auto& so = in.pSubobjects[i];
        if (so.Type != D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION || !so.pDesc) continue;
        const auto* c = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(so.pDesc);
        auto ci = Get(c->pExistingCollection);
        if (!ci) continue;
        for (const auto& p : ci->traceArgs)
            if (std::find(info->traceArgs.begin(), info->traceArgs.end(), p) == info->traceArgs.end())
                info->traceArgs.push_back(p);
        info->dynamicTraceArgs = info->dynamicTraceArgs || ci->dynamicTraceArgs;
        info->scenes.unknown = info->scenes.unknown || ci->scenes.unknown;
        for (const auto& r : ci->scenes.regs) AddScene(&info->scenes, r);
        for (const auto& h : ci->scenes.heap) AddHeapScene(&info->scenes, h);
        for (const auto& g : ci->groups) {
            std::vector<std::wstring> as;
            if (!c->NumExports) as.push_back(g.name);
            for (UINT e = 0; e < c->NumExports; ++e) {
                const auto& x = c->pExports[e];
                if ((x.ExportToRename ? x.ExportToRename : x.Name) == g.name) as.push_back(x.Name);
            }
            for (const auto& name : as) {
                Group ng = g;
                ng.name = name;
                info->groups.push_back(ng);
                info->recordBytes = (std::max)(info->recordBytes, g.offset + 4);
            }
        }
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
bool Extend(ID3D12Device* dev, ID3D12RootSignature* rs, bool srv,
            ComPtr<ID3D12RootSignature>* out, UINT* offset, std::string* why) {
    // Either the geometry index, a constant at b0, or the shim scene, a root
    // descriptor at t0, both in space kGeomIndexSpace.
    D3D12_ROOT_PARAMETER1 extra{};
    if (srv) {
        extra.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        extra.Descriptor.ShaderRegister = 0;
        extra.Descriptor.RegisterSpace = rq::kGeomIndexSpace;
    } else {
        extra.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        extra.Constants.ShaderRegister = 0;
        extra.Constants.RegisterSpace = rq::kGeomIndexSpace;
        extra.Constants.Num32BitValues = 1;
    }
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
    *offset = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + Align(ArgsEnd(before), srv ? 8 : 4);
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

// Extends the local root signature of each unit (exports that must share one:
// a hit group and its shaders, or a single raygen) with one parameter, the
// geometry index constant or (`srv`) the shim scene's root descriptor, and
// builds the new subobject array in `out`: libraries in `code` replaced,
// EXISTING_COLLECTIONs in `colls` pointed at a replacement, associations to a
// local root signature losing the extended exports (one left with none is
// dropped rather than becoming a DEFAULT association, and a signature left
// with no association is dropped rather than becoming a default one), and one
// new signature plus association per original signature extended.
// `offsets[u]` is the parameter's offset in unit u's records.
template <class T>
const void* Keep(Transformed* out, const T& v) {
    out->raw.emplace_back(sizeof(T));
    std::memcpy(out->raw.back().data(), &v, sizeof(T));
    return out->raw.back().data();
}

bool CarryLibrarySubobjects(ID3D12Device* dev, const D3D12_STATE_OBJECT_DESC& in,
                            const std::map<UINT, const std::vector<uint8_t>*>& code,
                            const std::map<UINT, LibSubobjects>& libSubs,
                            const std::set<std::wstring>& affected,
                            Transformed* out, std::string* why) {
    const UINT n = in.NumSubobjects;
    const D3D12_STATE_SUBOBJECT* s = in.pSubobjects;
    // What the state object associates itself, per subobject type.
    std::map<UINT, std::set<std::wstring>> soExplicit;
    std::set<UINT> soDefault;
    std::set<UINT> soHasType;
    std::vector<int> refd(n, 0);
    for (UINT i = 0; i < n; ++i) {
        soHasType.insert((UINT)s[i].Type);
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION || !s[i].pDesc)
            continue;
        const auto* a = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s[i].pDesc);
        const ptrdiff_t t = a->pSubobjectToAssociate - s;
        if (t < 0 || t >= (ptrdiff_t)n) continue;
        ++refd[t];
        const UINT ty = (UINT)s[t].Type;
        if (!a->NumExports) soDefault.insert(ty);
        for (UINT e = 0; e < a->NumExports; ++e) soExplicit[ty].insert(a->pExports[e]);
    }
    for (UINT i = 0; i < n; ++i) {
        const UINT ty = (UINT)s[i].Type;
        const bool associable = ty == 1 || ty == 2 || ty == 9 || ty == 10 || ty == 12;
        if (associable && !refd[i]) soDefault.insert(ty);
    }

    auto addAssoc = [&](size_t target, const std::vector<std::wstring>& ex) {
        std::vector<LPCWSTR> list;
        for (const auto& w : ex) {
            out->names.push_back(w);
            list.push_back(out->names.back().c_str());
        }
        out->exportLists.push_back(list);
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION na{};
        na.NumExports = (UINT)list.size();
        na.pExports = out->exportLists.back().data();
        na.pSubobjectToAssociate = reinterpret_cast<const D3D12_STATE_SUBOBJECT*>((uintptr_t)target);
        out->assoc.push_back(na);
        out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
                              &out->assoc.back() });
    };
    auto rootSig = [&](const std::vector<uint8_t>& bytes, ComPtr<ID3D12RootSignature>* rs) {
        std::vector<uint8_t> blob;
        if (!SerializeRts0(bytes, &blob, why)) return false;
        if (FAILED(dev->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(rs->GetAddressOf())))) {
            *why = "could not create a library's root signature";
            return false;
        }
        NoteRootSignature(rs->Get(), blob.data(), blob.size());
        out->sigs.push_back(*rs);
        return true;
    };

    for (const auto& kv : libSubs) {
        if (!code.count(kv.first)) continue;    // not rewritten: it keeps its own
        const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[kv.first].pDesc);
        const LibSubobjects& so = kv.second;
        if (ld->NumExports) {
            *why = "a library the shim rewrites declares subobjects and is included with an export "
                   "list, and the spec does not say which of its subobjects that includes";
            return false;
        }
        // Hit groups and the state object config are declared at once; an
        // associable subobject only when an association needs it, since one
        // left unassociated at state object scope would become a DEFAULT
        // there and reach exports it never reached.
        std::map<std::wstring, std::pair<const LibSubobjects::Sub*, long>> made;   // name -> (sub, index)
        for (const auto& x : so.all) {
            if (x.kind == 0) {
                if (soHasType.count(0)) continue;   // the state object's own wins
                out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG,
                                      Keep(out, D3D12_STATE_OBJECT_CONFIG{ (D3D12_STATE_OBJECT_FLAGS)x.w[0] }) });
                soHasType.insert(0);
            } else if (x.kind == 11) {
                D3D12_HIT_GROUP_DESC hg{};
                out->names.push_back(x.name);
                hg.HitGroupExport = out->names.back().c_str();
                hg.Type = (D3D12_HIT_GROUP_TYPE)x.w[0];
                LPCWSTR* slot[3] = { &hg.AnyHitShaderImport, &hg.ClosestHitShaderImport,
                                     &hg.IntersectionShaderImport };
                for (int k = 0; k < 3; ++k)
                    if (!x.str[k].empty()) {
                        out->names.push_back(x.str[k]);
                        *slot[k] = out->names.back().c_str();
                    }
                out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, Keep(out, hg) });
            } else if (x.kind == 1 || x.kind == 2 || x.kind == 9 || x.kind == 10 || x.kind == 12) {
                made[x.name] = { &x, -1 };
            }
        }
        auto ensure = [&](const std::wstring& name, long* index) -> bool {
            auto& m = made[name];
            if (m.second >= 0) { *index = m.second; return true; }
            const LibSubobjects::Sub& x = *m.first;
            const size_t at = out->subs.size();
            if (x.kind == 1 || x.kind == 2) {
                ComPtr<ID3D12RootSignature> rs;
                if (!rootSig(x.bytes, &rs)) return false;
                if (x.kind == 1) {
                    out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
                                          Keep(out, D3D12_GLOBAL_ROOT_SIGNATURE{ rs.Get() }) });
                } else {
                    out->lrs.push_back(D3D12_LOCAL_ROOT_SIGNATURE{ rs.Get() });
                    out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,
                                          &out->lrs.back() });
                }
            } else if (x.kind == 9) {
                out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,
                                      Keep(out, D3D12_RAYTRACING_SHADER_CONFIG{ x.w[0], x.w[1] }) });
            } else if (x.kind == 10) {
                out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
                                      Keep(out, D3D12_RAYTRACING_PIPELINE_CONFIG{ x.w[0] }) });
            } else {
                out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1,
                                      Keep(out, D3D12_RAYTRACING_PIPELINE_CONFIG1{
                                          x.w[0], (D3D12_RAYTRACING_PIPELINE_FLAGS)x.w[1] }) });
            }
            m.second = (long)at;
            *index = m.second;
            return true;
        };

        // This library's exports, for spelling its defaults out.
        std::vector<std::wstring> mine;
        for (const auto& f : Exported(ld)) mine.push_back(f.first);
        for (const auto& x : so.all)
            if (x.kind == 11) mine.push_back(x.name);

        std::map<UINT, std::set<std::wstring>> libExplicitOfType;
        std::set<std::wstring> associated;
        for (const auto& x : so.all) {
            if (x.kind != 8) continue;
            auto m = made.find(x.str[0]);
            if (m == made.end()) {
                bool here = false;
                for (const auto& y : so.all) here = here || y.name == x.str[0];
                if (!here) {
                    *why = "a library the shim rewrites associates a subobject declared elsewhere";
                    return false;
                }
                continue;   // a subobject of a kind nothing associates
            }
            associated.insert(x.str[0]);
            if (!x.exports.empty())
                for (const auto& e : x.exports) libExplicitOfType[m->second.first->kind].insert(e);
        }
        auto keepExport = [&](UINT ty, const std::wstring& e) {
            if (soExplicit[ty].count(e)) return false;
            if (ty == 2 && affected.count(e)) return false;   // extended, associated by the shim
            return true;
        };
        for (const auto& x : so.all) {
            if (x.kind != 8) continue;
            auto m = made.find(x.str[0]);
            if (m == made.end()) continue;
            const UINT ty = m->second.first->kind;
            if (soDefault.count(ty)) continue;   // the state object's default overrides it
            std::vector<std::wstring> ex;
            if (x.exports.empty()) {
                for (const auto& e : mine)
                    if (keepExport(ty, e) && !libExplicitOfType[ty].count(e)) ex.push_back(e);
            } else {
                for (const auto& e : x.exports)
                    if (keepExport(ty, e)) ex.push_back(e);
            }
            long at = -1;
            if (!ex.empty()) {
                if (!ensure(x.str[0], &at)) return false;
                addAssoc((size_t)at, ex);
            }
        }
        std::vector<std::wstring> names;
        for (const auto& kv2 : made) names.push_back(kv2.first);
        for (const auto& nm : names) {
            if (associated.count(nm)) continue;   // a default: nothing here names it
            const UINT ty = made[nm].first->kind;
            if (soDefault.count(ty)) continue;
            std::vector<std::wstring> ex;
            for (const auto& e : mine)
                if (keepExport(ty, e) && !libExplicitOfType[ty].count(e)) ex.push_back(e);
            long at = -1;
            if (!ex.empty()) {
                if (!ensure(nm, &at)) return false;
                addAssoc((size_t)at, ex);
            }
        }
    }
    return true;
}

bool Rebuild(ID3D12Device* dev, const D3D12_STATE_OBJECT_DESC& in, bool srv,
             const std::vector<std::vector<std::wstring>>& units,
             const std::map<UINT, const std::vector<uint8_t>*>& code,
             const std::map<UINT, ID3D12StateObject*>& colls,
             Transformed* out, std::vector<UINT>* offsets, UINT* signatures, std::string* why) {
    const UINT n = in.NumSubobjects;
    const D3D12_STATE_SUBOBJECT* s = in.pSubobjects;
    std::set<std::wstring> affected;
    for (const auto& u : units) for (const auto& w : u) affected.insert(w);

    std::map<std::wstring, UINT> explicitLrs;   // export -> subobject index
    std::vector<UINT> defaults;                 // default local root signatures
    std::vector<int> refs(n, 0);
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION && s[i].pDesc) {
            const auto* a = static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s[i].pDesc);
            for (UINT e = 0; e < a->NumExports; ++e)
                if (affected.count(a->pExports[e])) {
                    *why = "an extended export has a subobject associated from inside a "
                           "library, which is not handled yet";
                    return false;
                }
        }
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION || !s[i].pDesc)
            continue;
        const auto* a = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s[i].pDesc);
        const ptrdiff_t t = a->pSubobjectToAssociate - s;
        if (t < 0 || t >= (ptrdiff_t)n) {
            *why = "an association points outside the state object description";
            return false;
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

    // Local root signatures the LIBRARIES declare. The D3D12 spec, "Subobject
    // association behavior": an association declared directly in the state
    // object, explicit OR default, overrides any association a directly
    // included DXIL library makes for that export; a default declared inside
    // a library (a signature nothing in the library associates, or an
    // association with no exports) reaches that library's exports only.
    // Keyed -2 - k in `extended` below.
    std::vector<std::pair<std::wstring, std::vector<uint8_t>>> libLrs;
    std::map<std::wstring, int> libExplicit;    // export -> -2 - k
    std::map<std::wstring, int> libDefaultOf;   // export -> -2 - k
    std::map<UINT, LibSubobjects> libSubs;      // library subobject index -> its subobjects
    auto keyOf = [&](const std::wstring& name, const std::vector<uint8_t>& blob) {
        for (size_t k = 0; k < libLrs.size(); ++k)
            if (libLrs[k].first == name) return -2 - (int)k;
        libLrs.emplace_back(name, blob);
        return -2 - (int)(libLrs.size() - 1);
    };
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !s[i].pDesc) continue;
        const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc);
        LibSubobjects so;
        if (!LibrarySubobjects(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength, &so) ||
            so.all.empty())
            continue;
        std::set<std::wstring> associated;
        std::vector<std::wstring> defaultsHere;
        for (const auto& a : so.assoc) {
            auto l = so.lrs.find(a.first);
            if (l == so.lrs.end()) continue;
            associated.insert(a.first);
            if (a.second.empty()) { defaultsHere.push_back(a.first); continue; }
            const int key = keyOf(a.first, l->second);
            for (const auto& e : a.second) libExplicit[e] = key;
        }
        for (const auto& l : so.lrs)
            if (!associated.count(l.first)) defaultsHere.push_back(l.first);
        std::sort(defaultsHere.begin(), defaultsHere.end());
        defaultsHere.erase(std::unique(defaultsHere.begin(), defaultsHere.end()), defaultsHere.end());
        if (defaultsHere.size() > 1) {
            *why = "a library declares two default local root signatures";
            return false;
        }
        if (defaultsHere.size() == 1) {
            const int key = keyOf(defaultsHere[0], so.lrs[defaultsHere[0]]);
            for (const auto& f : Exported(ld)) libDefaultOf[f.first] = key;
            if (!ld->NumExports)
                for (const auto& x : so.all)
                    if (x.kind == 11) libDefaultOf[x.name] = key;
        }
        libSubs[i] = std::move(so);
    }

    // The first name decides when it is associated; the others must agree.
    // The state object's explicit associations, then its default, then a
    // library's explicit association, then a library's default.
    auto lrsOf = [&](const std::vector<std::wstring>& u, int* idx) -> bool {
        *idx = -1;
        auto it = explicitLrs.find(u[0]);
        if (it != explicitLrs.end()) { *idx = (int)it->second; return true; }
        for (size_t k = 1; k < u.size(); ++k) {
            auto jt = explicitLrs.find(u[k]);
            if (jt == explicitLrs.end()) continue;
            if (*idx >= 0 && *idx != (int)jt->second) return false;
            *idx = (int)jt->second;
        }
        if (*idx >= 0) return true;
        if (defaults.size() > 1) return false;
        if (defaults.size() == 1) { *idx = (int)defaults[0]; return true; }
        for (const std::map<std::wstring, int>* m : { &libExplicit, &libDefaultOf }) {
            bool found = false;
            for (const auto& w : u) {
                auto jt = m->find(w);
                if (jt == m->end()) continue;
                if (found && *idx != jt->second) return false;
                *idx = jt->second;
                found = true;
            }
            if (found) return true;
        }
        return true;
    };

    // One extended signature per original (or per "none", keyed -1).
    std::map<int, std::pair<ComPtr<ID3D12RootSignature>, UINT>> extended;
    std::map<int, std::vector<std::wstring>> assocNames;
    offsets->clear();
    for (const auto& u : units) {
        int idx;
        if (!lrsOf(u, &idx)) {
            *why = "cannot tell which local root signature " + Narrow(u[0]) + " has";
            return false;
        }
        if (!extended.count(idx)) {
            ID3D12RootSignature* orig = idx >= 0
                ? static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(s[idx].pDesc)->pLocalRootSignature
                : nullptr;
            ComPtr<ID3D12RootSignature> fromLib;
            if (idx <= -2) {
                // A library's own signature: made an object so it can be
                // extended like any other, its blob kept on it.
                std::vector<uint8_t> blob;
                if (!SerializeRts0(libLrs[(size_t)(-2 - idx)].second, &blob, why)) return false;
                if (FAILED(dev->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&fromLib)))) {
                    *why = "could not create a library's local root signature";
                    return false;
                }
                NoteRootSignature(fromLib.Get(), blob.data(), blob.size());
                orig = fromLib.Get();
            }
            ComPtr<ID3D12RootSignature> rs;
            UINT offset = 0;
            if (!Extend(dev, orig, srv, &rs, &offset, why)) return false;
            extended[idx] = { rs, offset };
        }
        offsets->push_back(extended[idx].second);
        auto& names = assocNames[idx];
        for (const auto& w : u)
            if (std::find(names.begin(), names.end(), w) == names.end()) names.push_back(w);
    }
    *signatures = (UINT)extended.size();

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
            auto c = code.find(i);
            if (c != code.end()) {
                out->code.push_back(*c->second);
                D3D12_DXIL_LIBRARY_DESC ld = *static_cast<const D3D12_DXIL_LIBRARY_DESC*>(so.pDesc);
                ld.DXILLibrary.pShaderBytecode = out->code.back().data();
                ld.DXILLibrary.BytecodeLength = out->code.back().size();
                out->libs.push_back(ld);
                so.pDesc = &out->libs.back();
            }
        } else if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
            auto c = colls.find(i);
            if (c != colls.end()) {
                D3D12_EXISTING_COLLECTION_DESC ec = *static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(so.pDesc);
                ec.pExistingCollection = c->second;
                out->colls.push_back(ec);
                so.pDesc = &out->colls.back();
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
    // A library the shim rewrites loses the subobjects it declares, because a
    // disassembly carries them only as comments. Each is declared again at
    // state object scope, and its associations become explicit ones with the
    // meaning the spec gives them: none where the state object associates
    // that type itself, since that already overrode the library's; a
    // library's default spelled out as that library's exports.
    if (!CarryLibrarySubobjects(dev, in, code, libSubs, affected, out, why)) return false;

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
    return true;
}

}  // namespace

void NoteRootSignature(ID3D12RootSignature* rs, const void* blob, size_t size) {
    if (rs && blob && size) rs->SetPrivateData(kBlobGuid, (UINT)size, blob);
}

void ScanScenes(const std::string& text, Scenes* out) { SceneRegs(text, out); }

bool ResolveScenes(const Scenes& sc, ID3D12RootSignature* rs, const std::vector<BoundRoot>& roots,
                   ID3D12DescriptorHeap* const* heaps, UINT numHeaps,
                   std::vector<D3D12_GPU_VIRTUAL_ADDRESS>* out, std::string* why) {
    out->clear();
    if (sc.unknown) {
        *why = "a TraceRay whose scene comes from a descriptor heap index, or could not be traced "
               "back to a declared resource";
        return false;
    }
    if (sc.regs.empty() && sc.heap.empty()) {
        *why = "no TraceRay scene register was found";
        return false;
    }
    if (!rs) { *why = "no compute root signature is bound"; return false; }
    UINT size = 0;
    if (FAILED(rs->GetPrivateData(kBlobGuid, &size, nullptr)) || !size) {
        *why = "a global root signature the shim did not see being created";
        return false;
    }
    std::vector<uint8_t> blob(size);
    rs->GetPrivateData(kBlobGuid, &size, blob.data());
    ComPtr<ID3D12VersionedRootSignatureDeserializer> des;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* v = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob.data(), blob.size(), IID_PPV_ARGS(&des))) ||
        FAILED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &v)) || !v) {
        *why = "could not read back the global root signature";
        return false;
    }
    ComPtr<ID3D12Device> dev;
    if (FAILED(rs->GetDevice(IID_PPV_ARGS(&dev)))) { *why = "no device"; return false; }
    const UINT64 inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto& d = v->Desc_1_1;
    char buf[160];
    for (const auto& reg : sc.regs) {
        if (!reg.count) {
            snprintf(buf, sizeof(buf), "TraceRay on an unbounded array of scenes at t%u space%u",
                     reg.lower, reg.space);
            *why = buf;
            return false;
        }
        for (UINT r = reg.lower; r < reg.lower + reg.count; ++r) {
            D3D12_GPU_VIRTUAL_ADDRESS a = 0;
            bool placed = false;
            for (UINT p = 0; p < d.NumParameters && !placed; ++p) {
                const auto& prm = d.pParameters[p];
                const BoundRoot b = p < roots.size() ? roots[p] : BoundRoot{};
                if (prm.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV) {
                    if (prm.Descriptor.RegisterSpace == reg.space && prm.Descriptor.ShaderRegister == r) {
                        placed = true;
                        a = b.srv;
                    }
                    continue;
                }
                if (prm.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
                UINT64 running = 0;
                for (UINT k = 0; k < prm.DescriptorTable.NumDescriptorRanges && !placed; ++k) {
                    const auto& rg = prm.DescriptorTable.pDescriptorRanges[k];
                    const UINT64 off = rg.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                           ? running : rg.OffsetInDescriptorsFromTableStart;
                    const UINT64 num = rg.NumDescriptors == UINT_MAX ? (1ull << 32) : rg.NumDescriptors;
                    running = off + num;
                    if (rg.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SRV || rg.RegisterSpace != reg.space ||
                        r < rg.BaseShaderRegister || r - rg.BaseShaderRegister >= num)
                        continue;
                    placed = true;
                    if (b.table) {
                        D3D12_GPU_DESCRIPTOR_HANDLE h{ b.table + (off + (r - rg.BaseShaderRegister)) * inc };
                        a = scenebind::Lookup(heaps, numHeaps, h);
                    }
                }
            }
            if (!placed || !a) {
                snprintf(buf, sizeof(buf), placed
                             ? "the scene at t%u space%u is in a descriptor the shim saw no structure written to"
                             : "the scene at t%u space%u is not in the global root signature (a local one?)",
                         r, reg.space);
                *why = buf;
                return false;
            }
            if (std::find(out->begin(), out->end(), a) == out->end()) out->push_back(a);
        }
    }
    for (const auto& hs : sc.heap) {
        // The heap index: a root constant, or a dword of a root CBV the CPU
        // can read. Read at record time, as the instance descriptions of an
        // upload heap are: what the application wrote before recording.
        bool have = false, placed = false;
        UINT slot = 0;
        for (UINT p = 0; p < d.NumParameters && !placed; ++p) {
            const auto& prm = d.pParameters[p];
            const BoundRoot b = p < roots.size() ? roots[p] : BoundRoot{};
            if (prm.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS &&
                prm.Constants.RegisterSpace == hs.space && prm.Constants.ShaderRegister == hs.reg) {
                placed = true;
                if (hs.offset % 4 == 0 && hs.offset / 4 < b.numConstants && b.constants) {
                    slot = b.constants[hs.offset / 4];
                    have = true;
                }
            } else if (prm.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV &&
                       prm.Descriptor.RegisterSpace == hs.space && prm.Descriptor.ShaderRegister == hs.reg) {
                placed = true;
                const restrack::Found f = b.cbv ? restrack::Find(b.cbv + hs.offset) : restrack::Found{};
                if (f.resource &&
                    (f.heap == D3D12_HEAP_TYPE_UPLOAD || f.heap == D3D12_HEAP_TYPE_READBACK)) {
                    uint8_t* mp = nullptr;
                    D3D12_RANGE rr{ (SIZE_T)f.offset, (SIZE_T)f.offset + 4 };
                    if (SUCCEEDED(f.resource->Map(0, &rr, reinterpret_cast<void**>(&mp))) && mp) {
                        std::memcpy(&slot, mp + f.offset, 4);
                        have = true;
                        D3D12_RANGE none{ 0, 0 };
                        f.resource->Unmap(0, &none);
                    }
                }
            }
        }
        if (!have) {
            snprintf(buf, sizeof(buf), placed
                         ? "the heap index of the scene (b%u space%u, byte %u) is not readable on the "
                           "CPU (a cbuffer in GPU-only memory, or unset)"
                         : "the heap index of the scene is in b%u space%u, not a root constant or root "
                           "CBV (a descriptor table, or a local root signature)",
                     hs.reg, hs.space, hs.offset);
            *why = buf;
            return false;
        }
        bool inHeap = false;
        const D3D12_GPU_VIRTUAL_ADDRESS a = scenebind::LookupSlot(heaps, numHeaps, slot, &inHeap);
        if (!a) {
            snprintf(buf, sizeof(buf), inHeap
                         ? "descriptor heap slot %u holds no structure the shim saw written"
                         : "descriptor heap slot %u is not in a bound shader-visible heap", slot);
            *why = buf;
            return false;
        }
        if (std::find(out->begin(), out->end(), a) == out->end()) out->push_back(a);
    }
    return true;
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
    if (rewritten.empty()) {
        // Nothing of its own to rewrite. A pipeline may still link collections
        // whose hit groups were extended, and a collection holding a raygen
        // records its TraceRay arguments for the pipelines that will link it.
        auto info = std::make_shared<Info>();
        MergeCollections(in, info.get());
        const bool collection = in.Type == D3D12_STATE_OBJECT_TYPE_COLLECTION;
        if (collection || !info->groups.empty()) {
            const UINT rec = Recursion(in);
            for (UINT i = 0; i < n; ++i)
                if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY && s[i].pDesc)
                    LibraryTraceArgs(static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc), rec,
                                     info.get());
        }
        if (info->groups.empty() && !(collection && (!info->traceArgs.empty() ||
                                                     info->dynamicTraceArgs)))
            return Outcome::kNothing;
        out->desc = in;
        out->info = info;
        if (!info->groups.empty())
            out->summary = std::to_string(info->groups.size()) +
                           " extended hit group(s) linked from collections";
        return Outcome::kTransformed;
    }

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
    std::set<std::wstring> covered;
    for (size_t k = 0; k < groups.size(); ++k)
        if (ext[k]) {
            covered.insert(groups[k].name);
            for (const auto& imp : groups[k].imports) covered.insert(imp);
        }
    // A reader no hit group here uses is still an export the driver compiles,
    // and it now reads a constant, so it needs the extended signature too.
    // Nothing can run it from this object, so no record carries its value.
    std::vector<std::wstring> loose;
    for (const auto& r : readers)
        if (!covered.count(r)) loose.push_back(r);

    // 3 to 5: extend and re-associate. Each group, with its shaders, is one
    //    unit; a loose reader is a unit alone.
    std::vector<std::vector<std::wstring>> units;
    std::vector<size_t> unitGroup;
    for (size_t k = 0; k < groups.size(); ++k) {
        if (!ext[k]) continue;
        std::vector<std::wstring> u{ groups[k].name };
        for (const auto& imp : groups[k].imports) u.push_back(imp);
        units.push_back(u);
        unitGroup.push_back(k);
    }
    for (const auto& r : loose) units.push_back({ r });
    // No unit: the readers are not exported here (a collection exporting only
    // a library's raygen, say). The library is still the rewritten one, and a
    // collection still records its TraceRay arguments below.
    std::map<UINT, const std::vector<uint8_t>*> code;
    for (const auto& L : rewritten) code[L.index] = &L.code;
    std::vector<UINT> offsets;
    UINT signatures = 0;
    if (!Rebuild(dev, in, false, units, code, {}, out, &offsets, &signatures, why))
        return Outcome::kRefused;
    auto info = std::make_shared<Info>();
    for (size_t u = 0; u < unitGroup.size(); ++u) {
        Group g;
        g.name = groups[unitGroup[u]].name;
        g.offset = offsets[u];
        info->groups.push_back(g);
        info->recordBytes = (std::max)(info->recordBytes, g.offset + 4);
    }

    // TraceRay arguments, from every library in the object that can trace,
    // and whatever the collections it links know.
    const UINT recursion = Recursion(in);
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !s[i].pDesc) continue;
        const Lib* r = nullptr;
        for (const auto& L : rewritten) if (L.index == i) r = &L;
        if (r) { TraceArgs(r->text, info.get()); continue; }
        LibraryTraceArgs(static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc), recursion,
                         info.get());
    }
    MergeCollections(in, info.get());
    out->info = info;

    std::string fns;
    for (const auto& w : readers) fns += (fns.empty() ? "" : ", ") + Narrow(w);
    std::string trace;
    for (const auto& p : info->traceArgs)
        trace += (trace.empty() ? "" : ", ") + std::string("(") + std::to_string(p.first) + ", " +
                 std::to_string(p.second) + ")";
    out->summary = "GeometryIndex() read in " + fns + "; " + std::to_string(info->groups.size()) +
                   " hit group(s) extended, " + std::to_string(signatures) +
                   " local root signature(s), record " + std::to_string(info->recordBytes) +
                   " bytes, TraceRay (R, M) " + (trace.empty() ? "none" : trace) +
                   (info->dynamicTraceArgs ? " plus some not known here" : "");
    return Outcome::kTransformed;
}

void Attach(ID3D12Device* dev, ID3D12StateObject* so, const D3D12_STATE_OBJECT_DESC& desc,
            const std::shared_ptr<Info>& info) {
    if (!so || !info) return;
    info->type = desc.Type;
    // A collection that traces is kept for the variants of the pipelines that
    // will link it; a pipeline with extended groups for its own variant.
    if ((desc.Type == D3D12_STATE_OBJECT_TYPE_COLLECTION && !info->traceArgs.empty()) ||
        !info->groups.empty()) {
        auto st = std::make_shared<StateObjectStore>();
        std::string why;
        if (st->Append(desc, false, why)) info->store = st;
        else ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): subobjects not kept (%s); no "
                      "variant can be built for this object\n", why.c_str());
    }
    ComPtr<ID3D12StateObjectProperties> props;
    if (SUCCEEDED(so->QueryInterface(IID_PPV_ARGS(&props))))
        for (auto& g : info->groups)
            if (void* id = props->GetShaderIdentifier(g.name.c_str()))
                std::memcpy(g.id, id, sizeof(g.id));
    auto* h = new InfoHolder(info);
    so->SetPrivateDataInterface(kInfoGuid, h);
    h->Release();
    // A multiplier of 0 puts every geometry of a structure on one record, so
    // the variant will be needed as soon as a structure holds two. Built now,
    // so the scene copies start with the next top-level build.
    if (desc.Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE && !info->groups.empty()) {
        bool zero = false;
        for (const auto& p : info->traceArgs) zero = zero || p.second == 0;
        std::string why;
        if (zero && !EnsureVariant(dev, *info, so, &why))
            ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): variant NOT built: %s\n", why.c_str());
    }
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
                 const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& boundSrvs, bool exact,
                 const void* owner, bool* shared, std::string* why) {
    *shared = false;
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
    // The labels come from the scene's instances as the CPU read them. Not
    // read yet, or read from an older build than the latest (GPU-written
    // instances are read every few builds, a submission late), they may put
    // the wrong geometry on a record: the variant is served on the GPU from
    // the latest build itself, so it takes those (0.51.0).
    bool read = false, stale = false;
    const auto labels =
        astrack::GeometryLabels(info.traceArgs, records, boundSrvs, exact, &read, &stale);
    if (!read || stale) {
        *why = !read ? (exact ? "the scene this dispatch traces has not been read yet"
                              : "no top-level structure has been read yet")
                     : "the scene changed since its instances were read";
        *shared = true;
        return false;
    }
    for (UINT r = 0; r < records; ++r)
        if (labels[r] == -2) {
            *why = "hit group record " + std::to_string(r) + " is reached by two different "
                   "geometries";
            *shared = true;
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


// --- the variant: the shim's own record layout -----------------------------------

namespace {

// A variant of the object `d` describes: every library that traces has its
// TraceRay calls pointed at the shim scene with (index of the pair, k), each
// raygen's local root signature gets the scene's root descriptor appended,
// and a linked collection that traces is replaced by its own variant.
// `raygens` receives (name, offset of the descriptor in its record), `names`
// every export whose identifier may have changed, both as `d` exports them.
bool VariantOf(ID3D12Device* dev, const D3D12_STATE_OBJECT_DESC& d,
               const std::vector<std::pair<UINT, UINT>>& pairs,
               ComPtr<ID3D12StateObject>* out,
               std::vector<std::pair<std::wstring, UINT>>* raygens,
               std::vector<std::wstring>* names,
               std::vector<ComPtr<ID3D12StateObject>>* parts, std::string* why) {
    const UINT n = d.NumSubobjects;
    const D3D12_STATE_SUBOBJECT* s = d.pSubobjects;
    const UINT recursion = Recursion(d);
    std::deque<std::vector<uint8_t>> codeStore;
    std::map<UINT, const std::vector<uint8_t>*> code;
    std::map<UINT, ID3D12StateObject*> colls;
    std::vector<std::vector<std::wstring>> units;
    std::vector<std::pair<unsigned, unsigned>> upairs(pairs.begin(), pairs.end());
    bool any = false;
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP && s[i].pDesc) {
            const auto* h = static_cast<const D3D12_HIT_GROUP_DESC*>(s[i].pDesc);
            if (h->HitGroupExport) names->push_back(h->HitGroupExport);
        } else if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY && s[i].pDesc) {
            const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc);
            const auto fns = Exported(ld);
            if (GetEnvironmentVariableA("DXR_TIER11_GI_DEBUG", nullptr, 0)) {
                std::string l;
                for (const auto& f : fns) l += Narrow(f.first) + ":" + std::to_string(f.second) + " ";
                ProxyLog("[gi-debug] library %u exports %zu: %s(NumExports %u)\n", i, fns.size(),
                         l.c_str(), ld->NumExports);
            }
            bool raygen = false, tracingOther = false;
            for (const auto& f : fns) {
                names->push_back(f.first);
                if (f.second == 7) raygen = true;
                if ((f.second == 10 || f.second == 11) && recursion != 1) tracingOther = true;
            }
            if (!raygen && !tracingOther) continue;
            std::string text, err, lowered;
            if (!dxch::Disassemble(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength,
                                   &text, &err)) {
                *why = "could not disassemble a tracing library: " + err;
                return false;
            }
            int calls = 0;
            if (!rq::RetraceToShimScene(llm::Normalize(text), upairs, &lowered, &calls, why))
                return false;
            if (!calls) continue;
            if (tracingOther) {
                *why = "a closest-hit or miss shader that calls TraceRay (recursion above 1)";
                return false;
            }
            codeStore.emplace_back();
            if (!dxch::AssembleAndSign(lowered, &codeStore.back(), &err)) {
                *why = "the variant library did not assemble: " + err;
                return false;
            }
            code[i] = &codeStore.back();
            for (const auto& f : fns)
                if (f.second == 7) units.push_back({ f.first });
            any = true;
        } else if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION && s[i].pDesc) {
            const auto* c = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s[i].pDesc);
            auto ci = Get(c->pExistingCollection);
            if (GetEnvironmentVariableA("DXR_TIER11_GI_DEBUG", nullptr, 0))
                ProxyLog("[gi-debug] collection %u: info %d store %d trace %zu\n", i, ci ? 1 : 0,
                         ci && ci->store ? 1 : 0, ci ? ci->traceArgs.size() : (size_t)0);
            if (!ci || !ci->store || (ci->traceArgs.empty() && !ci->dynamicTraceArgs)) continue;
            ComPtr<ID3D12StateObject> vc;
            std::vector<std::pair<std::wstring, UINT>> subRg;
            std::vector<std::wstring> subNames;
            D3D12_STATE_OBJECT_DESC cd = ci->store->Desc(D3D12_STATE_OBJECT_TYPE_COLLECTION);
            if (!VariantOf(dev, cd, pairs, &vc, &subRg, &subNames, parts, why)) return false;
            if (!vc) continue;
            parts->push_back(vc);
            colls[i] = vc.Get();
            // Under the names this object links them by.
            auto as = [&](const std::wstring& w, std::vector<std::wstring>* o) {
                if (!c->NumExports) { o->push_back(w); return; }
                for (UINT e = 0; e < c->NumExports; ++e)
                    if ((c->pExports[e].ExportToRename ? c->pExports[e].ExportToRename
                                                       : c->pExports[e].Name) == w)
                        o->push_back(c->pExports[e].Name);
            };
            for (const auto& w : subNames) as(w, names);
            for (const auto& r : subRg) {
                std::vector<std::wstring> o;
                as(r.first, &o);
                for (const auto& w : o) raygens->emplace_back(w, r.second);
            }
            any = true;
        }
    }
    if (!any) {
        // A collection with nothing that traces is linked as it is.
        if (d.Type == D3D12_STATE_OBJECT_TYPE_COLLECTION) { out->Reset(); return true; }
        *why = "no TraceRay was found to point at the shim scene";
        return false;
    }
    Transformed t;
    std::vector<UINT> offsets;
    UINT sigs = 0;
    if (!Rebuild(dev, d, true, units, code, colls, &t, &offsets, &sigs, why)) return false;
    for (size_t u = 0; u < units.size(); ++u) raygens->emplace_back(units[u][0], offsets[u]);
    ComPtr<ID3D12Device5> d5;
    HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&d5));
    if (SUCCEEDED(hr)) hr = d5->CreateStateObject(&t.desc, IID_PPV_ARGS(out->GetAddressOf()));
    if (FAILED(hr)) {
        char b[64];
        std::snprintf(b, sizeof(b), "the variant's CreateStateObject failed, hr=0x%08lX", (unsigned long)hr);
        *why = b;
        return false;
    }
    return true;
}

struct TablePipe {
    ID3D12Device* device = nullptr;
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
};
TablePipe g_tablePipe;

}  // namespace

bool EnsureVariant(ID3D12Device* dev, Info& info, ID3D12StateObject* app, std::string* why) {
    std::lock_guard<std::mutex> lk(info.variantLock);
    if (info.variantTried) {
        if (!info.variant) *why = info.variantWhy;
        return info.variant != nullptr;
    }
    info.variantTried = true;
    auto fail = [&](const std::string& w) { info.variantWhy = w; *why = w; return false; };
    if (!info.store) return fail("the pipeline's subobjects were not kept");
    if (info.dynamicTraceArgs) return fail("TraceRay arguments not known when the pipeline was created");
    if (info.traceArgs.empty()) return fail("the pipeline has no TraceRay");
    if (info.traceArgs.size() > 15) return fail("more than 15 TraceRay argument pairs");
    const D3D12_STATE_OBJECT_DESC d = info.store->Desc(info.type);
    std::vector<std::pair<std::wstring, UINT>> raygens;
    std::vector<std::wstring> names;
    std::string w;
    if (!VariantOf(dev, d, info.traceArgs, &info.variant, &raygens, &names, &info.variantParts, &w))
        return fail(w);
    ComPtr<ID3D12StateObjectProperties> pa, pv;
    if (FAILED(app->QueryInterface(IID_PPV_ARGS(&pa))) ||
        FAILED(info.variant->QueryInterface(IID_PPV_ARGS(&pv)))) {
        info.variant.Reset();
        return fail("no state object properties");
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (const auto& r : raygens)
        if (std::find(names.begin(), names.end(), r.first) == names.end()) names.push_back(r.first);
    for (const auto& name : names) {
        void* from = pa->GetShaderIdentifier(name.c_str());
        void* to = pv->GetShaderIdentifier(name.c_str());
        if (!from || !to) continue;
        Remap m;
        std::memcpy(m.from, from, sizeof(m.from));
        std::memcpy(m.to, to, sizeof(m.to));
        for (const auto& r : raygens)
            if (r.first == name) m.insert = r.second;
        if (m.insert || std::memcmp(m.from, m.to, sizeof(m.from)) != 0) info.remaps.push_back(m);
        if (m.insert) info.raygenBytes = (std::max)(info.raygenBytes, m.insert + 8);
    }
    shimscene::Activate((UINT)info.traceArgs.size());
    ProxyLog("[dxr-tier-11-proxy-log] GeometryIndex(): VARIANT pipeline built, the shim's own "
             "record layout: %zu raygen(s) tracing the shim scene, %zu identifier(s) remapped, "
             "%zu collection(s) rebuilt\n", raygens.size(), info.remaps.size(), info.variantParts.size());
    return true;
}

bool RecordVariant(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, Info& info,
                   const D3D12_DISPATCH_RAYS_DESC& app, D3D12_DISPATCH_RAYS_DESC* mine,
                   const std::vector<D3D12_GPU_VIRTUAL_ADDRESS>& boundSrvs, bool exact,
                   const void* owner, std::string* why) {
    // The scene this dispatch traces, and the shim's copy of its latest
    // build: made with the build, or at this dispatch from the instances the
    // shim saved then or the CPU snapshot (0.51.0; until then a scene built on
    // the GPU before the variant existed was refused, and one built once was
    // never drawn). The variant traces ONE copy, so every TraceRay has to
    // trace the same, resolved, scene.
    if (!exact) {
        *why = "the scene this dispatch traces is not resolved through the root signature, and "
               "the variant traces a copy of exactly that scene";
        return false;
    }
    if (boundSrvs.size() != 1) {
        *why = "its TraceRay calls trace different scenes, and the variant has one scene copy";
        return false;
    }
    const UINT k = (UINT)info.traceArgs.size();
    shimscene::Copy cp;
    std::string cw;
    if (!shimscene::Ensure(cl, dev, boundSrvs[0], k, owner, &cp, &cw)) {
        *why = "no copy of the scene this dispatch traces: " + cw;
        return false;
    }
    gpuhold::Use(cp.tlasRes, owner);
    gpuhold::Use(cp.contribRes, owner);

    std::lock_guard<std::mutex> g(g_lock);
    if (g_tablePipe.device != dev || !g_tablePipe.pso) {
        TablePipe p;
        p.device = dev;
        if (FAILED(dev->CreateRootSignature(0, g_shimTableCS, sizeof(g_shimTableCS), IID_PPV_ARGS(&p.rs)))) {
            *why = "could not create the variant table root signature";
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = p.rs.Get();
        pd.CS.pShaderBytecode = g_shimTableCS;
        pd.CS.BytecodeLength = sizeof(g_shimTableCS);
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&p.pso)))) {
            *why = "could not create the variant table pipeline";
            return false;
        }
        g_tablePipe = p;
    }
    const UINT groups = (UINT)info.groups.size(), remaps = (UINT)info.remaps.size();
    const UINT metaDwords = groups * 9 + remaps * 17 + 2 * k;
    auto meta = Acquire(dev, D3D12_HEAP_TYPE_UPLOAD, (UINT64)metaDwords * 4, owner);
    if (!meta) { *why = "could not create the variant table metadata"; return false; }
    uint32_t* m = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(meta->Map(0, &none, reinterpret_cast<void**>(&m)))) {
        *why = "could not map the variant table metadata";
        return false;
    }
    for (UINT x = 0; x < groups; ++x) {
        std::memcpy(m + x * 9, info.groups[x].id, 32);
        m[x * 9 + 8] = info.groups[x].offset;
    }
    for (UINT y = 0; y < remaps; ++y) {
        uint32_t* e = m + groups * 9 + y * 17;
        std::memcpy(e, info.remaps[y].from, 32);
        std::memcpy(e + 8, info.remaps[y].to, 32);
        e[16] = info.remaps[y].insert;
    }
    for (UINT q = 0; q < k; ++q) {
        m[groups * 9 + remaps * 17 + 2 * q] = info.traceArgs[q].first;
        m[groups * 9 + remaps * 17 + 2 * q + 1] = info.traceArgs[q].second;
    }
    meta->Unmap(0, nullptr);

    static const bool poison = GetEnvironmentVariableA("DXR_TIER11_GI_POISON", nullptr, 0) != 0;
    cl->SetComputeRootSignature(g_tablePipe.rs.Get());
    cl->SetPipelineState(g_tablePipe.pso.Get());
    cl->SetComputeRootShaderResourceView(2, meta->GetGPUVirtualAddress());
    cl->SetComputeRootShaderResourceView(3, cp.contrib);
    // One table: the application's range in, the shim's out.
    auto table = [&](D3D12_GPU_VIRTUAL_ADDRESS src, UINT srcStride, UINT records, UINT dstStride,
                     UINT mode, UINT appRecords, UINT per) -> ComPtr<ID3D12Resource> {
        auto dst = Acquire(dev, D3D12_HEAP_TYPE_DEFAULT, (UINT64)records * dstStride, owner);
        if (!dst) return nullptr;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = dst.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cl->ResourceBarrier(1, &b);
        const UINT c[13] = { records, srcStride, dstStride, groups, remaps, mode, k, cp.gmax,
                             appRecords, (UINT)(cp.tlas & 0xFFFFFFFFull), (UINT)(cp.tlas >> 32), per,
                             poison ? 1u : 0u };
        cl->SetComputeRoot32BitConstants(0, 13, c, 0);
        cl->SetComputeRootShaderResourceView(1, src);
        cl->SetComputeRootUnorderedAccessView(4, dst->GetGPUVirtualAddress());
        cl->Dispatch((records + 63) / 64, 1, 1);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &b);
        return dst;
    };
    *mine = app;
    const auto A32 = [](UINT64 v) { return (UINT)((v + 31) / 32 * 32); };

    // Raygen: one record, the variant's identifier, the scene's address.
    const UINT rgSrc = (UINT)app.RayGenerationShaderRecord.SizeInBytes;
    const UINT rgDst = A32((std::max)((UINT64)rgSrc, (UINT64)info.raygenBytes));
    auto rg = table(app.RayGenerationShaderRecord.StartAddress, rgSrc, 1, rgDst, 0, 1, 1);
    if (!rg) { *why = "could not create the variant raygen record"; return false; }
    mine->RayGenerationShaderRecord = { rg->GetGPUVirtualAddress(), rgDst };

    // Miss and callable: identifiers remapped, layout unchanged.
    auto plain = [&](const D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE& in,
                     D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE* o) -> bool {
        if (!in.SizeInBytes) return true;
        const UINT stride = in.StrideInBytes ? (UINT)in.StrideInBytes : A32(in.SizeInBytes);
        const UINT recs = (UINT)(in.SizeInBytes / stride);
        auto t = table(in.StartAddress, stride, recs, stride, 0, recs, 1);
        if (!t) return false;
        *o = { t->GetGPUVirtualAddress(), (UINT64)recs * stride, in.StrideInBytes };
        return true;
    };
    if (!plain(app.MissShaderTable, &mine->MissShaderTable) ||
        !plain(app.CallableShaderTable, &mine->CallableShaderTable)) {
        *why = "could not create the variant miss or callable table";
        return false;
    }

    // Hit groups: the shim's own layout.
    const UINT appStride = (UINT)app.HitGroupTable.StrideInBytes;
    if (!appStride) { *why = "a hit group table with stride 0"; return false; }
    const UINT appRecords = (UINT)(app.HitGroupTable.SizeInBytes / appStride);
    const UINT per = cp.k * cp.gmax;
    const UINT records = cp.count * per;
    const UINT dstStride = A32((std::max)(appStride, info.recordBytes));
    auto hit = table(app.HitGroupTable.StartAddress, appStride, records, dstStride, 1, appRecords, per);
    if (!hit) { *why = "could not create the variant hit group table"; return false; }
    mine->HitGroupTable = { hit->GetGPUVirtualAddress(), (UINT64)records * dstStride, dstStride };
    return true;
}

}  // namespace gidx
