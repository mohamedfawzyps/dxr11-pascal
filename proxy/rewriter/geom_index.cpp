// C++ port of phase5/rewriter/geomidx.py. Mirrors its structure on purpose, so
// the two can be held byte-identical; see geom_index.h.
#include "geom_index.h"

#include "ll_model.h"

#include <algorithm>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace rq {

// In rq_lower.cpp: resolves #RQRO and friends against the module.
std::string ResolveAttrs(const std::string& in, std::string* err);

namespace {

const unsigned long long kTier11Flag = 0x2000000ull;
const char* kHandle = "%dx.types.Handle";
const char* kType = "%dxr11.gi";
const char* kGlobal = "@dxr11.gi";
const char* kName = "dxr11.gi";
const char* kCbufRet = "%dx.types.CBufRet.i32";
const char* kResProps = "%dx.types.ResourceProperties";

bool StartsWith(const std::string& s, const std::string& p) {
    return s.compare(0, p.size(), p) == 0;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> SplitTrim(const std::string& s) {
    std::vector<std::string> r;
    size_t at = 0;
    for (;;) {
        size_t c = s.find(',', at);
        r.push_back(Trim(s.substr(at, c == std::string::npos ? std::string::npos : c - at)));
        if (c == std::string::npos) break;
        at = c + 1;
    }
    return r;
}

std::string Join(const std::vector<std::string>& v, const char* sep) {
    std::string r;
    for (size_t k = 0; k < v.size(); ++k) {
        if (k) r += sep;
        r += v[k];
    }
    return r;
}

bool IsSm66(const std::string& text) {
    static const std::regex kSm(R"RX(^!\d+ = !\{!"(?:cs|lib)", i32 (\d+), i32 (\d+)\})RX");
    for (const auto& line : llm::SplitLines(text)) {
        std::smatch mm;
        if (std::regex_search(line, mm, kSm)) {
            const int maj = std::stoi(mm[1].str()), min = std::stoi(mm[2].str());
            return maj > 6 || (maj == 6 && min >= 6);
        }
    }
    return false;
}

std::vector<std::string> Read(bool sm66, int n, const std::string& result) {
    const std::string t = "%gi." + std::to_string(n);
    const std::string H = kHandle, T = kType, G = kGlobal, C = kCbufRet;
    if (sm66) {
        return {
            "  " + t + ".v = load " + H + ", " + H + "* " + G + ", align 4",
            "  " + t + ".l = call " + H + " @dx.op.createHandleForLib.dx.types.Handle(i32 160, " +
                H + " " + t + ".v)  ; CreateHandleForLib(Resource)",
            "  " + t + ".h = call " + H + " @dx.op.annotateHandle(i32 216, " + H + " " + t +
                ".l, " + kResProps + " { i32 13, i32 4 })  ; AnnotateHandle(res,props)  resource: CBuffer",
            "  " + t + ".r = call " + C + " @dx.op.cbufferLoadLegacy.i32(i32 59, " + H + " " + t +
                ".h, i32 0)  ; CBufferLoadLegacy(handle,regIndex)",
            "  " + result + " = extractvalue " + C + " " + t + ".r, 0",
        };
    }
    return {
        "  " + t + ".v = load " + T + ", " + T + "* " + G + ", align 4",
        "  " + t + ".h = call " + H + " @dx.op.createHandleForLib." + kName + "(i32 160, " +
            T + " " + t + ".v)  ; CreateHandleForLib(Resource)",
        "  " + t + ".r = call " + C + " @dx.op.cbufferLoadLegacy.i32(i32 59, " + H + " " + t +
            ".h, i32 0)  ; CBufferLoadLegacy(handle,regIndex)",
        "  " + result + " = extractvalue " + C + " " + t + ".r, 0",
    };
}

bool Metadata(std::string* text, bool sm66, std::string* why) {
    auto lines = llm::SplitLines(*text);
    static const std::regex kNode(R"RX(^!(\d+) = (?:distinct )?!\{(.*)\}\s*$)RX");
    std::map<int, std::string> md;
    for (const auto& l : lines) {
        std::smatch m;
        if (std::regex_match(l, m, kNode)) md[std::stoi(m[1].str())] = m[2].str();
    }
    int nxt = md.empty() ? 0 : md.rbegin()->first + 1;
    std::vector<std::string> added;
    auto node = [&](const std::string& body) {
        const int id = nxt++;
        added.push_back("!" + std::to_string(id) + " = !{" + body + "}");
        return id;
    };
    auto rewrite = [&](int nid, const std::string& body) {
        const std::regex pat("^!" + std::to_string(nid) + R"RX( = !\{.*\}\s*$)RX");
        for (auto& l : lines)
            if (std::regex_match(l, pat)) l = "!" + std::to_string(nid) + " = !{" + body + "}";
    };
    const std::string H = kHandle, T = kType, G = kGlobal;
    const std::string gref = sm66 ? T + "* bitcast (" + H + "* " + G + " to " + T + "*)"
                                  : T + "* " + G;

    int entry = -1;
    bool haveEp = false;
    static const std::regex kEp(R"RX(^!dx\.entryPoints = !\{(.*)\}\s*$)RX");
    static const std::regex kRes(R"RX(^!dx\.resources = !\{!(\d+)\}\s*$)RX");
    int rid = -1;
    for (const auto& l : lines) {
        std::smatch m;
        if (std::regex_match(l, m, kEp)) {
            haveEp = true;
            for (const auto& ref : SplitTrim(m[1].str())) {
                const int nid = std::stoi(ref.substr(1));
                if (SplitTrim(md[nid])[0] == "null") entry = nid;
            }
        } else if (std::regex_match(l, m, kRes)) {
            rid = std::stoi(m[1].str());
        }
    }
    if (!haveEp) { *why = "no !dx.entryPoints"; return false; }
    if (entry < 0) { *why = "no module entry in !dx.entryPoints"; return false; }
    auto fields = SplitTrim(md[entry]);
    const std::string space = std::to_string(kGeomIndexSpace);

    if (rid >= 0) {
        auto groups = SplitTrim(md[rid]);
        std::vector<std::string> have;
        if (groups[2] != "null") have = SplitTrim(md[std::stoi(groups[2].substr(1))]);
        const int rec = node("i32 " + std::to_string(have.size()) + ", " + gref + ", !\"" + kName +
                             "\", i32 " + space + ", i32 0, i32 1, i32 4, null");
        have.push_back("!" + std::to_string(rec));
        groups[2] = "!" + std::to_string(node(Join(have, ", ")));
        rewrite(rid, Join(groups, ", "));
    } else {
        const int rec = node("i32 0, " + gref + ", !\"" + kName + "\", i32 " + space +
                             ", i32 0, i32 1, i32 4, null");
        const int list = node("!" + std::to_string(rec));
        rid = node("null, null, !" + std::to_string(list) + ", null");
        for (size_t k = 0; k < lines.size(); ++k)
            if (StartsWith(lines[k], "!dx.entryPoints = ")) {
                lines.insert(lines.begin() + k, "!dx.resources = !{!" + std::to_string(rid) + "}");
                break;
            }
        fields[3] = "!" + std::to_string(rid);
    }

    if (fields[4] != "null") {
        const int pid = std::stoi(fields[4].substr(1));
        auto props = SplitTrim(md[pid]);
        for (size_t k = 0; k + 1 < props.size(); k += 2) {
            if (props[k] == "i32 0") {
                const unsigned long long flags =
                    std::stoull(props[k + 1].substr(props[k + 1].find(' ') + 1)) & ~kTier11Flag;
                if (flags) props[k + 1] = "i64 " + std::to_string(flags);
                else props.erase(props.begin() + k, props.begin() + k + 2);
                break;
            }
        }
        if (!props.empty()) rewrite(pid, Join(props, ", "));
        else fields[4] = "null";
    }
    rewrite(entry, Join(fields, ", "));
    std::string s = llm::JoinLines(lines);
    while (!s.empty() && s.back() == '\n') s.pop_back();
    *text = s + "\n" + Join(added, "\n") + "\n";
    return true;
}

}  // namespace

std::string ExportName(const std::string& fn) {
    static const std::regex kMangled(R"RX(^\\01\?([^@]+)@@)RX");
    std::smatch m;
    if (std::regex_search(fn, m, kMangled)) return m[1].str();
    return fn;
}

bool LowerGeometryIndex(const std::string& in, std::string* out,
                        std::vector<std::string>* functions, std::string* why) {
    static const std::regex kCall(R"RX(^(\s*)(%[\w.]+) = call i32 @dx\.op\.geometryIndex\.i32\(i32 213\))RX");
    functions->clear();
    auto lines = llm::SplitLines(in);
    bool any = false;
    for (const auto& l : lines) {
        std::smatch m;
        if (std::regex_search(l, m, kCall)) { any = true; break; }
    }
    if (!any) { *out = in; return true; }
    if (in.find("@dx.op.rayQuery_") != std::string::npos ||
        in.find("@dx.op.allocateRayQuery") != std::string::npos) {
        *why = "the library reads GeometryIndex() and also uses RayQuery; "
               "RayQuery inside DXR shaders has no lowering yet";
        return false;
    }
    if (in.find(std::string(kGlobal) + " ") != std::string::npos ||
        in.find(std::string(kType) + " ") != std::string::npos) {
        *why = std::string("the library already defines ") + kGlobal;
        return false;
    }
    const std::regex kSpace("i32 " + std::to_string(kGeomIndexSpace) + R"RX(, i32 \d+, i32 \d+, i32 \d+)RX");
    if (std::regex_search(in, kSpace)) {
        *why = "the library already binds register space " + std::to_string(kGeomIndexSpace);
        return false;
    }
    const bool sm66 = IsSm66(in);

    static const std::regex kDefine(R"RX(^define [^@]*@"?([^"(]+)"?\()RX");
    std::vector<std::string> cur;
    std::string fn;
    int n = 0;
    for (const auto& l : lines) {
        std::smatch m;
        if (std::regex_search(l, m, kDefine)) fn = m[1].str();
        if (std::regex_search(l, m, kCall)) {
            for (auto& r : Read(sm66, n, m[2].str())) cur.push_back(r);
            ++n;
            const std::string name = ExportName(fn);
            if (std::find(functions->begin(), functions->end(), name) == functions->end())
                functions->push_back(name);
            continue;
        }
        cur.push_back(l);
    }
    lines.swap(cur);

    cur.clear();
    for (size_t k = 0; k < lines.size(); ++k) {
        const auto& l = lines[k];
        if (StartsWith(l, "declare i32 @dx.op.geometryIndex.i32(")) {
            if (!cur.empty() && StartsWith(cur.back(), "; Function Attrs:")) cur.pop_back();
            if (!cur.empty() && cur.back().empty() && k + 1 < lines.size() && lines[k + 1].empty())
                cur.pop_back();
            continue;
        }
        cur.push_back(l);
    }
    lines.swap(cur);
    const std::string body = llm::JoinLines(lines);
    const std::string H = kHandle, C = kCbufRet;
    std::vector<std::string> decls;
    if (body.find("declare " + C + " @dx.op.cbufferLoadLegacy.i32(") == std::string::npos)
        decls.push_back("declare " + C + " @dx.op.cbufferLoadLegacy.i32(i32, " + H + ", i32) #RQRO");
    if (sm66) {
        if (body.find("declare " + H + " @dx.op.createHandleForLib.dx.types.Handle(") == std::string::npos)
            decls.push_back("declare " + H + " @dx.op.createHandleForLib.dx.types.Handle(i32, " + H + ") #RQRO");
        if (body.find("declare " + H + " @dx.op.annotateHandle(") == std::string::npos)
            decls.push_back("declare " + H + " @dx.op.annotateHandle(i32, " + H + ", " + kResProps +
                            ") #RQNONE");
    } else {
        decls.push_back("declare " + H + " @dx.op.createHandleForLib." + kName + "(i32, " + kType +
                        ") #RQRO");
    }
    int lastDecl = -1, lastClose = -1;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (StartsWith(lines[k], "declare ")) lastDecl = (int)k;
        if (lines[k] == "}") lastClose = (int)k;
    }
    if (lastDecl < 0) lastDecl = lastClose;
    std::vector<std::string> ins;
    for (const auto& d : decls) { ins.push_back(""); ins.push_back(d); }
    lines.insert(lines.begin() + lastDecl + 1, ins.begin(), ins.end());

    auto haveType = [&](const std::string& t) {
        for (const auto& l : lines) if (StartsWith(l, t + " = type")) return true;
        return false;
    };
    std::vector<std::string> types;
    if (!haveType(H)) types.push_back(H + " = type { i8* }");
    if (!haveType(C)) types.push_back(C + " = type { i32, i32, i32, i32 }");
    if (sm66 && !haveType(kResProps)) types.push_back(std::string(kResProps) + " = type { i32, i32 }");
    types.push_back(std::string(kType) + " = type { i32 }");
    static const std::regex kTypeLine(R"RX(^%[\w.]+ = type )RX");
    static const std::regex kGlobalLine(R"RX(^@[\w.]+ = )RX");
    int lastType = -1;
    for (size_t k = 0; k < lines.size(); ++k)
        if (std::regex_search(lines[k], kTypeLine)) lastType = (int)k;
    lines.insert(lines.begin() + lastType + 1, types.begin(), types.end());
    int lastGlobal = -1;
    for (size_t k = 0; k < lines.size(); ++k)
        if (std::regex_search(lines[k], kGlobalLine)) lastGlobal = (int)k;
    const int at = (lastGlobal >= 0 ? lastGlobal : lastType + (int)types.size()) + 1;
    std::vector<std::string> g;
    if (lastGlobal < 0) g.push_back("");
    g.push_back(std::string(kGlobal) + " = external constant " + (sm66 ? H : std::string(kType)));
    lines.insert(lines.begin() + at, g.begin(), g.end());

    std::string text = llm::JoinLines(lines);
    if (!Metadata(&text, sm66, why)) return false;
    std::string err;
    *out = ResolveAttrs(text, &err);
    if (!err.empty()) { *why = err; return false; }
    return true;
}

// --- shimtrace.py -------------------------------------------------------------

bool RetraceToShimScene(const std::string& in,
                        const std::vector<std::pair<unsigned, unsigned>>& pairs, unsigned copies,
                        const std::vector<unsigned>& slotOf, unsigned slots,
                        std::string* out, int* calls, std::string* why) {
    static const std::regex kTrace(
        R"RX(^(\s*)call void @dx\.op\.traceRay\.([^(]+)\(i32 157, %dx\.types\.Handle (%[\w.]+), i32 ([^,]+), i32 ([^,]+), i32 ([^,]+), i32 ([^,]+), (.*)$)RX");
    static const std::regex kNum(R"RX(^-?\d+$)RX");
    static const std::regex kLabel(R"RX(^([A-Za-z$._][\w$.]*):)RX");
    const std::string H = kHandle, RAS = "%struct.RaytracingAccelerationStructure",
                      BAB = "%struct.ByteAddressBuffer", RR = "%dx.types.ResRet.i32",
                      G = "@dxr11.tlas", L = "@dxr11.lut", P = kResProps;
    *calls = 0;
    auto lines = llm::SplitLines(in);
    bool any = false;
    for (const auto& l : lines) if (std::regex_match(l, kTrace)) { any = true; break; }
    if (!any) { *out = in; return true; }
    if (in.find(G + " ") != std::string::npos || in.find(G + ".") != std::string::npos ||
        in.find(L + " ") != std::string::npos) {
        *why = "the library already defines " + G;
        return false;
    }
    const bool sm66 = IsSm66(in);
    const unsigned k = (unsigned)pairs.size();
    if (!slots) slots = 1;
    const unsigned C = copies ? copies : 1u;
    // Slot j's structure c: @dxr11.tlas for slot 0 structure 0, @dxr11.tlas.c
    // after it; @dxr11.tlas.sJ and @dxr11.tlas.sJ.c for slot j.
    auto global = [&](unsigned j, unsigned c) {
        const std::string b = j ? G + ".s" + std::to_string(j) : G;
        return c ? b + "." + std::to_string(c) : b;
    };
    unsigned slot = 0;   // the call being rewritten's
    // A handle on structure c of the current call's slot, named <t><suffix>.
    auto tlasHandle = [&](std::vector<std::string>* o, const std::string& t, const std::string& sfx,
                          unsigned c) {
        if (sm66) {
            o->push_back("  " + t + ".v" + sfx + " = load " + H + ", " + H + "* " + global(slot, c) + ", align 4");
            o->push_back("  " + t + ".l" + sfx + " = call " + H + " @dx.op.createHandleForLib.dx.types.Handle(i32 160, " +
                          H + " " + t + ".v" + sfx + ")  ; CreateHandleForLib(Resource)");
            o->push_back("  " + t + ".h" + sfx + " = call " + H + " @dx.op.annotateHandle(i32 216, " + H + " " + t +
                         ".l" + sfx + ", " + P + " { i32 16, i32 0 })  ; AnnotateHandle(res,props)  resource: RTAccelerationStructure");
        } else {
            o->push_back("  " + t + ".v" + sfx + " = load " + RAS + ", " + RAS + "* " + global(slot, c) + ", align 4");
            o->push_back("  " + t + ".h" + sfx + " = call " + H +
                         " @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure(i32 160, " +
                         RAS + " " + t + ".v" + sfx + ")  ; CreateHandleForLib(Resource)");
        }
    };
    std::vector<std::string> cur;
    int n = 0;
    // Splitting a block moves its way out to the new join block: a phi
    // naming the block as a predecessor then names the join (per function).
    std::string chain;                       // the label the current block started with
    std::map<std::string, std::string> renamed;
    size_t fnStart = 0;
    for (const auto& l : lines) {
        std::smatch m;
        if (StartsWith(l, "define ")) {
            chain.clear();
            renamed.clear();
            fnStart = cur.size();
        } else if (std::regex_search(l, m, kLabel)) {
            chain = m[1].str();
        } else if (l == "}" && !renamed.empty()) {
            for (size_t i = fnStart; i < cur.size(); ++i) {
                if (cur[i].find(" = phi ") == std::string::npos) continue;
                for (const auto& r : renamed) {
                    const std::string from = ", %" + r.first + " ]", to = ", %" + r.second + " ]";
                    for (size_t at = cur[i].find(from); at != std::string::npos;
                         at = cur[i].find(from, at + to.size()))
                        cur[i].replace(at, from.size(), to);
                }
            }
        }
        if (!std::regex_match(l, m, kTrace)) { cur.push_back(l); continue; }
        if (!slotOf.empty() && (size_t)n >= slotOf.size()) {
            *why = "more TraceRay calls than scene slots given";
            return false;
        }
        slot = slotOf.empty() ? 0u : slotOf[(size_t)n];
        if (slot >= slots) { *why = "a TraceRay's scene slot is out of range"; return false; }
        const std::string R = m[6].str(), M = m[7].str();
        const std::string t = "%st." + std::to_string(n);
        const std::string head = m[1].str() + "call void @dx.op.traceRay." + m[2].str() + "(i32 157, " + H + " ";
        const std::string mid = ", i32 " + m[4].str() + ", i32 " + m[5].str() + ", i32 ";
        const std::string tail = m[8].str();
        if (!copies) {
            if (!std::regex_match(R, kNum) || !std::regex_match(M, kNum)) {
                *why = "a TraceRay whose hit group arguments are computed at run time";
                return false;
            }
            const std::pair<unsigned, unsigned> pr((unsigned)std::stol(R) & 15u,
                                                   (unsigned)std::stol(M) & 15u);
            auto it = std::find(pairs.begin(), pairs.end(), pr);
            if (it == pairs.end()) {
                *why = "a TraceRay argument pair the pipeline did not list";
                return false;
            }
            tlasHandle(&cur, t, "", 0);
            cur.push_back(head + t + ".h" + mid + std::to_string(it - pairs.begin()) + ", i32 " +
                          std::to_string(k) + ", " + tail);
            ++n;
            continue;
        }
        // The pair read from the shim's table at 16 * (R & 15) + (M & 15):
        // r in bits 0-7, the multiplier in 8-15, the scene copy in 16 up.
        cur.push_back("  " + t + ".r = and i32 " + R + ", 15");
        cur.push_back("  " + t + ".m = and i32 " + M + ", 15");
        cur.push_back("  " + t + ".s = shl i32 " + t + ".r, 4");
        cur.push_back("  " + t + ".k = or i32 " + t + ".s, " + t + ".m");
        cur.push_back("  " + t + ".o = shl i32 " + t + ".k, 2");
        if (sm66) {
            cur.push_back("  " + t + ".lv = load " + H + ", " + H + "* " + L + ", align 4");
            cur.push_back("  " + t + ".ll = call " + H + " @dx.op.createHandleForLib.dx.types.Handle(i32 160, " +
                          H + " " + t + ".lv)  ; CreateHandleForLib(Resource)");
            cur.push_back("  " + t + ".lh = call " + H + " @dx.op.annotateHandle(i32 216, " + H + " " + t +
                          ".ll, " + P + " { i32 11, i32 0 })  ; AnnotateHandle(res,props)  resource: ByteAddressBuffer");
        } else {
            cur.push_back("  " + t + ".lv = load " + BAB + ", " + BAB + "* " + L + ", align 4");
            cur.push_back("  " + t + ".lh = call " + H + " @dx.op.createHandleForLib.struct.ByteAddressBuffer(i32 160, " +
                          BAB + " " + t + ".lv)  ; CreateHandleForLib(Resource)");
        }
        cur.push_back("  " + t + ".lr = call " + RR + " @dx.op.rawBufferLoad.i32(i32 139, " + H + " " + t +
                      ".lh, i32 " + t + ".o, i32 undef, i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)");
        cur.push_back("  " + t + ".e = extractvalue " + RR + " " + t + ".lr, 0");
        cur.push_back("  " + t + ".q = and i32 " + t + ".e, 255");
        cur.push_back("  " + t + ".x = lshr i32 " + t + ".e, 8");
        cur.push_back("  " + t + ".kk = and i32 " + t + ".x, 255");
        const std::string args = mid + t.substr(0) + ".q, i32 " + t + ".kk, " + tail;
        if (copies == 1) {
            tlasHandle(&cur, t, "", 0);
            cur.push_back(head + t + ".h" + args);
            ++n;
            continue;
        }
        const std::string lab = "st." + std::to_string(n);
        cur.push_back("  " + t + ".c = lshr i32 " + t + ".e, 16");
        cur.push_back("  switch i32 " + t + ".c, label %" + lab + ".b0 [");
        for (unsigned c = 1; c < copies; ++c)
            cur.push_back("    i32 " + std::to_string(c) + ", label %" + lab + ".b" + std::to_string(c));
        cur.push_back("  ]");
        for (unsigned c = 0; c < copies; ++c) {
            const std::string sfx = std::to_string(c);
            cur.push_back("");
            cur.push_back(lab + ".b" + sfx + ":");
            tlasHandle(&cur, t, sfx, c);
            cur.push_back(head + t + ".h" + sfx + args);
            cur.push_back("  br label %" + lab + ".j");
        }
        cur.push_back("");
        cur.push_back(lab + ".j:");
        if (!chain.empty()) renamed[chain] = lab + ".j";
        ++n;
    }
    lines.swap(cur);

    const std::string body = llm::JoinLines(lines);
    std::vector<std::string> decls;
    auto declare = [&](const std::string& name, const std::string& d) {
        if (body.find(name) == std::string::npos) decls.push_back(d);
    };
    if (sm66) {
        declare("declare " + H + " @dx.op.createHandleForLib.dx.types.Handle(",
                "declare " + H + " @dx.op.createHandleForLib.dx.types.Handle(i32, " + H + ") #RQRO");
        declare("declare " + H + " @dx.op.annotateHandle(",
                "declare " + H + " @dx.op.annotateHandle(i32, " + H + ", " + P + ") #RQNONE");
    } else {
        declare("declare " + H + " @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure(",
                "declare " + H + " @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure(i32, " +
                    RAS + ") #RQRO");
        if (copies)
            declare("declare " + H + " @dx.op.createHandleForLib.struct.ByteAddressBuffer(",
                    "declare " + H + " @dx.op.createHandleForLib.struct.ByteAddressBuffer(i32, " + BAB + ") #RQRO");
    }
    if (copies)
        declare("declare " + RR + " @dx.op.rawBufferLoad.i32(",
                "declare " + RR + " @dx.op.rawBufferLoad.i32(i32, " + H + ", i32, i32, i8, i32) #RQRO");
    int lastDecl = -1;
    for (size_t i = 0; i < lines.size(); ++i) if (StartsWith(lines[i], "declare ")) lastDecl = (int)i;
    std::vector<std::string> ins;
    for (const auto& d : decls) { ins.push_back(""); ins.push_back(d); }
    lines.insert(lines.begin() + lastDecl + 1, ins.begin(), ins.end());

    auto haveType = [&](const std::string& ty) {
        for (const auto& l : lines) if (StartsWith(l, ty + " = type")) return true;
        return false;
    };
    std::vector<std::string> types;
    if (!haveType(H)) types.push_back(H + " = type { i8* }");
    if (sm66 && !haveType(P)) types.push_back(P + " = type { i32, i32 }");
    if (!haveType(RAS)) types.push_back(RAS + " = type { i32 }");
    if (copies && !haveType(BAB)) types.push_back(BAB + " = type { i32 }");
    if (copies && !haveType(RR)) types.push_back(RR + " = type { i32, i32, i32, i32, i32 }");
    static const std::regex kTypeLine(R"RX(^%[\w.]+ = type )RX");
    static const std::regex kGlobalLine(R"RX(^@[\w."\\?@]+ = )RX");
    int lastType = -1;
    for (size_t i = 0; i < lines.size(); ++i)
        if (std::regex_search(lines[i], kTypeLine)) lastType = (int)i;
    lines.insert(lines.begin() + lastType + 1, types.begin(), types.end());
    int lastGlobal = -1;
    for (size_t i = 0; i < lines.size(); ++i)
        if (std::regex_search(lines[i], kGlobalLine)) lastGlobal = (int)i;
    const int at = (lastGlobal >= 0 ? lastGlobal : lastType + (int)types.size()) + 1;
    std::vector<std::string> g;
    if (lastGlobal < 0) g.push_back("");
    for (unsigned j = 0; j < slots; ++j)
        for (unsigned c = 0; c < C; ++c)
            g.push_back(global(j, c) + " = external constant " + (sm66 ? H : RAS) + ", align 4");
    if (copies) g.push_back(L + " = external constant " + (sm66 ? H : BAB) + ", align 4");
    lines.insert(lines.begin() + at, g.begin(), g.end());

    // Metadata: SRV records, kind 16 for each copy and 11 for the table, in
    // the resources' SRV group.
    static const std::regex kNode(R"RX(^!(\d+) = (?:distinct )?!\{(.*)\}\s*$)RX");
    std::map<int, std::string> md;
    for (const auto& l : lines) {
        std::smatch m;
        if (std::regex_match(l, m, kNode)) md[std::stoi(m[1].str())] = m[2].str();
    }
    int nxt = md.empty() ? 0 : md.rbegin()->first + 1;
    std::vector<std::string> added;
    auto node = [&](const std::string& b) {
        const int id = nxt++;
        added.push_back("!" + std::to_string(id) + " = !{" + b + "}");
        return id;
    };
    auto rewrite = [&](int nid, const std::string& b) {
        const std::regex pat("^!" + std::to_string(nid) + R"RX( = !\{.*\}\s*$)RX");
        for (auto& l : lines)
            if (std::regex_match(l, pat)) l = "!" + std::to_string(nid) + " = !{" + b + "}";
    };
    auto ref = [&](const std::string& ty, const std::string& gl) {
        return sm66 ? ty + "* bitcast (" + H + "* " + gl + " to " + ty + "*)" : ty + "* " + gl;
    };
    static const std::regex kEp(R"RX(^!dx\.entryPoints = !\{(.*)\}\s*$)RX");
    static const std::regex kRes(R"RX(^!dx\.resources = !\{!(\d+)\}\s*$)RX");
    int entry = -1, rid = -1;
    bool haveEp = false;
    for (const auto& l : lines) {
        std::smatch m;
        if (std::regex_match(l, m, kEp)) {
            haveEp = true;
            for (const auto& r : SplitTrim(m[1].str())) {
                const int nid = std::stoi(r.substr(1));
                if (SplitTrim(md[nid])[0] == "null") entry = nid;
            }
        } else if (std::regex_match(l, m, kRes)) {
            rid = std::stoi(m[1].str());
        }
    }
    if (!haveEp) { *why = "no !dx.entryPoints"; return false; }
    if (entry < 0) { *why = "no module entry in !dx.entryPoints"; return false; }
    auto fields = SplitTrim(md[entry]);
    const std::string space = std::to_string(kGeomIndexSpace);
    const int extra = node("i32 0, i32 4");
    std::vector<std::string> have;
    std::vector<std::string> groups;
    if (rid >= 0) {
        groups = SplitTrim(md[rid]);
        if (groups[0] != "null") have = SplitTrim(md[std::stoi(groups[0].substr(1))]);
    }
    const size_t first = have.size();
    for (unsigned j = 0; j < slots; ++j)
        for (unsigned c = 0; c < C; ++c) {
            const unsigned r = j * C + c;
            const std::string name = global(j, c).substr(1);
            const int rec = node("i32 " + std::to_string(first + r) + ", " + ref(RAS, global(j, c)) + ", !\"" +
                                 name + "\", i32 " + space + ", i32 " + std::to_string(r) +
                                 ", i32 1, i32 16, i32 0, !" + std::to_string(extra));
            have.push_back("!" + std::to_string(rec));
        }
    if (copies) {
        const unsigned r = slots * C;
        const int rec = node("i32 " + std::to_string(first + r) + ", " + ref(BAB, L) +
                             ", !\"dxr11.lut\", i32 " + space + ", i32 " + std::to_string(r) +
                             ", i32 1, i32 11, i32 0, null");
        have.push_back("!" + std::to_string(rec));
    }
    if (rid >= 0) {
        groups[0] = "!" + std::to_string(node(Join(have, ", ")));
        rewrite(rid, Join(groups, ", "));
    } else {
        const int list = node(Join(have, ", "));
        rid = node("!" + std::to_string(list) + ", null, null, null");
        for (size_t i = 0; i < lines.size(); ++i)
            if (StartsWith(lines[i], "!dx.entryPoints = ")) {
                lines.insert(lines.begin() + i, "!dx.resources = !{!" + std::to_string(rid) + "}");
                break;
            }
        fields[3] = "!" + std::to_string(rid);
        rewrite(entry, Join(fields, ", "));
    }
    std::string s = llm::JoinLines(lines);
    while (!s.empty() && s.back() == '\n') s.pop_back();
    std::string err;
    *out = ResolveAttrs(s + "\n" + Join(added, "\n") + "\n", &err);
    if (!err.empty()) { *why = err; return false; }
    *calls = n;
    return true;
}

}  // namespace rq
