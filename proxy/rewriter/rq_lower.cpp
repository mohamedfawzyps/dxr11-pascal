#include "rq_lower.h"

#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>

namespace rq {
namespace {

const char* kPayload = "%struct.Payload";
const char* kAttrs = "%struct.BuiltInTriangleIntersectionAttributes";

// One payload shape carrying every committed accessor the whitelist supports.
// Tailoring it per shader would save a few bytes and cost a lot of ways to get
// the offsets wrong; the brief puts correctness well ahead of that.
//   0 float t | 1 <2 x float> bary | 2 i32 hit | 3 inst | 4 prim | 5 geom
//   0 t | 1 bary | 2 hit | 3 inst | 4 prim | 5 unused | 6 instanceID
//   7 hitKind | 8 worldToObject, 12 floats
//
// The LAYOUT is fixed even though the matrix is only sometimes read: variable
// offsets across two implementations is a good way to get one of them subtly
// wrong. The per-invocation COST is what is made conditional instead.
const char* kPayloadType =
    "{ float, <2 x float>, i32, i32, i32, i32, i32, i32, [12 x float] }";
const int kPayloadBytes = 84;
// HitKind() is an integer; the RayQuery form is a bool.
const int kHitKindFront = 254;

struct Field { int idx; const char* ty; int align; };

std::optional<Field> PayloadField(int op) {
    switch (op) {
        case kCommittedRayT:            return Field{ 0, "float", 8 };
        case kCommittedBary:            return Field{ 1, "<2 x float>", 4 };
        case kCommittedStatus:          return Field{ 2, "i32", 4 };
        case kCommittedInstanceIndex:   return Field{ 3, "i32", 4 };
        case kCommittedPrimitiveIndex:  return Field{ 4, "i32", 4 };
        case kCommittedInstanceID:      return Field{ 6, "i32", 4 };
        case kCommittedFrontFace:       return Field{ 7, "i32", 4 };
        case kCommittedWorldToObject:   return Field{ 8, "[12 x float]", 4 };
        default:                        return std::nullopt;
    }
}

// Which dx.op gives each field in a DXR 1.0 closest-hit, read off DXC output.
// Field 5, geometry index, is deliberately absent: emitting dx.op.geometryIndex
// would set shader flag 0x2000000 and the driver would refuse the state object.
const std::pair<int, const char*> kChSource[] = {
    { 3, "call i32 @dx.op.instanceIndex.i32(i32 142)" },
    { 4, "call i32 @dx.op.primitiveIndex.i32(i32 161)" },
    { 6, "call i32 @dx.op.instanceID.i32(i32 141)" },
    { 7, "call i32 @dx.op.hitKind.i32(i32 143)" },
};

// RayQuery -> DXR 1.0, for accessors read in the ANY-HIT shader. Measured
// from DXC output on both sides. The operand shape is identical apart from the
// query handle, which simply goes away.
struct CandMap { int op; const char* ty; const char* callee; int dxop; int extra; };
const CandMap kCandidateMap[] = {
    { kCandidateInstanceIndex,  "i32",   "dx.op.instanceIndex.i32",     142, 0 },
    { kCandidateInstanceID,     "i32",   "dx.op.instanceID.i32",        141, 0 },
    { kCandidatePrimitiveIndex, "i32",   "dx.op.primitiveIndex.i32",    161, 0 },
    { kCandidateRayT,           "float", "dx.op.rayTCurrent.f32",       154, 0 },
    { kCandidateObjectRayOrigin,    "float", "dx.op.objectRayOrigin.f32",    149, 1 },
    { kCandidateObjectRayDirection, "float", "dx.op.objectRayDirection.f32", 150, 1 },
    { kCandidateWorldToObject,      "float", "dx.op.worldToObject.f32",      152, 2 },
};
const CandMap* FindCand(int op) {
    for (const auto& c : kCandidateMap) if (c.op == op) return &c;
    return nullptr;
}

struct ResRec {
    int nid = 0;
    std::string gty;      // global type, "[4 x %..]" for an array
    std::string elem;     // element type
    bool isArray = false;
    std::string name;
};

struct GlobalInfo {
    int cls = 0;
    int nid = 0;
    std::string sym;
    std::string gty;
    std::string elem;
    std::string name;
};

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> SplitTop(const std::string& s) { return llm::SplitArgs(s); }

std::map<int, std::string> Metadata(const std::string& text) {
    static const std::regex re(R"(!(\d+) = !\{(.*)\}\s*)");
    std::map<int, std::string> out;
    for (const auto& line : llm::SplitLines(text)) {
        std::smatch m;
        if (std::regex_match(line, m, re))
            out[std::stoi(m[1].str())] = m[2].str();
    }
    return out;
}

// The createHandleForLib overload name for a resource type. DXC names it after
// the type with the leading % dropped, and quotes the whole symbol when the
// type does.
std::string HandleFn(const std::string& llvmType) {
    std::string bare = llvmType.substr(1);
    if (!bare.empty() && bare.front() == '"') {
        std::string inner = bare.substr(1, bare.size() - 2);
        return "\"dx.op.createHandleForLib." + inner + "\"";
    }
    return "dx.op.createHandleForLib." + bare;
}

std::string Replace(std::string s, const std::string& from, const std::string& to) {
    size_t p = s.find(from);
    if (p != std::string::npos) s.replace(p, from.size(), to);
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------

LowerResult Lower(const llm::Module& m, const Query& q, const Exports& e) {
    LowerResult r;
    const std::string& text0 = m.text;
    const auto md = Metadata(text0);

    // --- resource table ----------------------------------------------------
    std::smatch rm;
    static const std::regex kRes(R"(!dx\.resources\s*=\s*!\{!(\d+)\})");
    if (!std::regex_search(text0, rm, kRes)) {
        r.error = "module has no !dx.resources";
        return r;
    }
    const std::string resId = rm[1].str();
    std::map<int, std::vector<ResRec>> table;   // class -> records, in order
    {
        auto groups = SplitTop(md.at(std::stoi(resId)));
        static const std::regex kType(R"RX((\[[^\]]+\]|%[\w.$-]+|%"[^"]+")\*\s+\S+)RX");
        static const std::regex kArr(R"(^\[(\d+) x (.+)\]$)");
        static const std::regex kName(R"RX(^!"([^"]*)")RX");
        for (size_t cls = 0; cls < groups.size(); ++cls) {
            const std::string g = Trim(groups[cls]);
            if (g == "null") continue;
            std::vector<ResRec> recs;
            for (const auto& idTok : SplitTop(md.at(std::stoi(g.substr(1))))) {
                const int nid = std::stoi(Trim(idTok).substr(1));
                auto fields = SplitTop(md.at(nid));
                std::smatch tm;
                if (fields.size() < 3 || !std::regex_match(fields[1], tm, kType)) {
                    r.error = "resource record !" + std::to_string(nid) +
                              " has an unexpected shape";
                    return r;
                }
                ResRec rec;
                rec.nid = nid;
                rec.gty = tm[1].str();
                std::smatch am;
                if (std::regex_match(rec.gty, am, kArr)) {
                    rec.isArray = true;
                    rec.elem = am[2].str();
                } else {
                    rec.elem = rec.gty;
                }
                std::smatch nm;
                if (std::regex_search(fields[2], nm, kName)) rec.name = nm[1].str();
                recs.push_back(rec);
            }
            table[static_cast<int>(cls)] = recs;
        }
    }

    // --- one global per resource, in the same order Python builds them -----
    std::vector<GlobalInfo> globals;
    {
        static const std::regex kIdent(R"(^[A-Za-z_]\w*$)");
        const char* clsName[] = { "srv", "uav", "cbv", "smp" };
        for (const auto& kv : table) {
            for (size_t n = 0; n < kv.second.size(); ++n) {
                const ResRec& rec = kv.second[n];
                GlobalInfo g;
                g.cls = kv.first;
                g.nid = rec.nid;
                if (!rec.name.empty() && std::regex_match(rec.name, kIdent))
                    g.sym = rec.name;
                else
                    g.sym = std::string("rq_") +
                            (kv.first >= 0 && kv.first < 4 ? clsName[kv.first] : "res") +
                            std::to_string(n);
                g.gty = rec.gty;
                g.elem = rec.elem;
                g.name = rec.name.empty() ? g.sym : rec.name;
                globals.push_back(g);
            }
        }
    }
    auto findGlobal = [&](int cls, int nid) -> const GlobalInfo* {
        for (const auto& g : globals)
            if (g.cls == cls && g.nid == nid) return &g;
        return nullptr;
    };

    // The load + createHandleForLib that replaces one createHandle. Shared,
    // because the any-hit shader has to recreate any resource handle the
    // Proceed loop body used: a handle is not caller state, it names a
    // resource, and every shader in the library can reach it.
    std::string handleErr;
    auto handleText = [&](const llm::Instr& instr) -> std::string {
        static const std::regex kI8(R"(i8\s+(\d+))");
        static const std::regex kI32(R"(i32\s+(\d+))");
        static const std::regex kI32End(R"(^i32\s+(\d+)$)");
        std::smatch mm;
        std::string a1 = Trim(instr.args[1]), a2 = Trim(instr.args[2]);
        std::regex_search(a1, mm, kI8);
        const int cls = std::stoi(mm[1].str());
        std::regex_search(a2, mm, kI32);
        const int rid = std::stoi(mm[1].str());

        auto it = table.find(cls);
        if (it == table.end() || rid >= static_cast<int>(it->second.size())) {
            handleErr = "createHandle names range " + std::to_string(rid) +
                        " of class " + std::to_string(cls) +
                        ", which !dx.resources does not describe";
            return "";
        }
        const GlobalInfo* g = findGlobal(cls, it->second[rid].nid);
        const std::string fn = HandleFn(g->elem);
        const std::string raw = "%rq.raw." + instr.result.substr(1);

        std::string src;
        if (g->gty == g->elem) {
            src = g->elem + "* @" + g->sym;
        } else {
            // A resource array. DXC reaches the element through a constant
            // getelementptr on the array global, and hands createHandleForLib
            // the ELEMENT type. Matched against a DXC-built library.
            std::string a3 = Trim(instr.args[3]);
            std::smatch im;
            if (!std::regex_match(a3, im, kI32End)) {
                handleErr = "resource array indexed by a non-constant (" + a3 +
                            "); dynamic descriptor indexing is not supported";
                return "";
            }
            const std::string nonuni = Trim(instr.args[4]);
            if (nonuni != "i1 false" && nonuni != "i1 0") {
                handleErr = "resource array indexed non-uniformly; not supported";
                return "";
            }
            src = g->elem + "* getelementptr inbounds (" + g->gty + ", " + g->gty +
                  "* @" + g->sym + ", i32 0, i32 " + im[1].str() + ")";
        }
        return "  " + raw + " = load " + g->elem + ", " + src + ", align 4\n" +
               "  " + instr.result + " = call %dx.types.Handle @" + fn +
               "(i32 160, " + g->elem + " " + raw + ")  ; CreateHandleForLib(Resource)";
    };

    std::map<int, std::optional<std::string>> edits;
    const llm::Function& fn = *q.fn;

    // --- resources ---------------------------------------------------------
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (i.DxOp() == kCreateHandle) {
                const std::string t = handleText(i);
                if (!handleErr.empty()) { r.error = handleErr; return r; }
                edits[i.index] = t;
            }

    // --- ThreadId is not legal in a raygen ---------------------------------
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (i.DxOp() == kThreadId) {
                static const std::regex kI32(R"(i32\s+(\d+))");
                std::smatch mm;
                std::string a1 = Trim(i.args[1]);
                std::regex_search(a1, mm, kI32);
                edits[i.index] = "  " + i.result +
                    " = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 " +
                    mm[1].str() + ")  ; DispatchRaysIndex(col)";
            }

    // --- the payload alloca has to be in the entry block --------------------
    {
        const llm::Block& entry = fn.blocks.front();
        if (entry.instrs.empty()) { r.error = "entry block is empty"; return r; }
        const llm::Instr& first = entry.instrs.front();
        std::string pending = first.line;
        auto it = edits.find(first.index);
        if (it != edits.end() && it->second.has_value()) pending = *it->second;
        edits[first.index] = std::string("  %rq.pl = alloca ") + kPayload +
                             ", align 8\n" + pending;
    }

    edits[q.alloc->index] = std::nullopt;

    // --- the loop, if there is one -----------------------------------------
    std::string exitLabel;
    std::string preheader;
    if (q.hasLoop) {
        const llm::Block* latchBlk = fn.FindBlock(q.loop.latch);
        std::vector<std::string> outs;
        for (const auto& s : latchBlk->Successors())
            if (!q.loop.body.count(s)) outs.push_back(s);
        if (outs.size() != 1) {
            r.error = "Proceed loop has " + std::to_string(outs.size()) +
                      " exits; expected one";
            return r;
        }
        exitLabel = outs[0];

        // The any-hit shader is a separate invocation with only the payload
        // for shared state, so the loop body must not read caller locals or
        // leak values back out. A resource HANDLE is exempt: it names a
        // resource the any-hit shader can reach for itself.
        std::set<std::string> defined, used, handles;
        for (const auto& lbl : q.loop.body) {
            const llm::Block* b = fn.FindBlock(lbl);
            if (!b) continue;
            for (const auto& i : b->instrs) {
                if (!i.result.empty()) defined.insert(i.result);
                for (const auto& u : i.Uses()) used.insert(u);
            }
        }
        for (const auto& b : fn.blocks)
            for (const auto& i : b.instrs)
                if (i.DxOp() == kCreateHandle && !i.result.empty())
                    handles.insert(i.result);
        std::vector<std::string> outside;
        for (const auto& u : used)
            if (!defined.count(u) && u != q.handle && !handles.count(u))
                outside.push_back(u);
        if (!outside.empty()) {
            std::string list;
            for (size_t i = 0; i < outside.size(); ++i) {
                if (i) list += ", ";
                list += outside[i];
            }
            r.error = "Proceed loop body reads values defined outside it (" + list +
                      "); the any-hit shader is a separate invocation and the payload "
                      "is the only shared state";
            return r;
        }
        for (const auto& b : fn.blocks) {
            if (q.loop.body.count(b.label)) continue;
            for (const auto& i : b.instrs)
                for (const auto& u : i.Uses())
                    if (defined.count(u)) {
                        r.error = "value " + u + " defined in the Proceed loop is used "
                                  "after it; the any-hit shader cannot return it";
                        return r;
                    }
        }

        // The block the guard branches through to reach the loop, if any.
        for (const auto& p : q.proceeds) {
            if (p.first->label == q.loop.latch) continue;
            for (const auto& s : p.first->Successors()) {
                if (q.loop.body.count(s)) continue;
                const llm::Block* blk = fn.FindBlock(s);
                if (blk) {
                    auto succ = blk->Successors();
                    if (succ.size() == 1 && succ[0] == q.loop.header) preheader = s;
                }
            }
            break;
        }
    }

    // --- payload init plus the TraceRay that replaces the inline query ------
    {
        std::vector<std::string> lines;
        const struct { int idx; const char* ty; int align; } init[] = {
            { 0, "float", 8 }, { 1, "<2 x float>", 4 }, { 2, "i32", 4 },
            { 3, "i32", 4 }, { 4, "i32", 4 }, { 5, "i32", 4 },
            { 6, "i32", 4 }, { 7, "i32", 4 }, { 8, "[12 x float]", 4 },
        };
        for (const auto& f : init) {
            const std::string zero =
                std::string(f.ty) == "float" ? "0.000000e+00"
                : ((f.ty[0] == '<' || f.ty[0] == '[') ? "zeroinitializer" : "0");
            lines.push_back("  %rq.pl" + std::to_string(f.idx) +
                            " = getelementptr inbounds " + kPayload + ", " + kPayload +
                            "* %rq.pl, i32 0, i32 " + std::to_string(f.idx));
            lines.push_back(std::string("  store ") + f.ty + " " + zero + ", " + f.ty +
                            "* %rq.pl" + std::to_string(f.idx) + ", align " +
                            std::to_string(f.align));
        }
        const auto ra = q.RayArgs();
        std::ostringstream tr;
        tr << "  call void @dx.op.traceRay." << std::string(kPayload).substr(1)
           << "(i32 157, %dx.types.Handle " << q.AsHandle() << ", i32 " << q.RayFlags()
           << ", " << ra[0] << ", i32 0, i32 0, i32 0, " << ra[1] << ", " << ra[2]
           << ", " << ra[3] << ", " << ra[4] << ", " << ra[5] << ", " << ra[6]
           << ", " << ra[7] << ", " << ra[8] << ", " << kPayload
           << "* nonnull %rq.pl)  ; TraceRay(...)";
        lines.push_back(tr.str());
        if (!exitLabel.empty()) lines.push_back("  br label " + exitLabel);
        std::string joined;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) joined += "\n";
            joined += lines[i];
        }
        edits[q.trace->index] = joined;
    }

    // --- Proceed calls vanish ----------------------------------------------
    for (const auto& p : q.proceeds) {
        edits[p.second->index] = std::nullopt;
        if (q.hasLoop && p.first->label == q.loop.latch) continue;
        const llm::Instr* term = p.first->Terminator();
        if (term && term != p.second && !edits.count(term->index)) {
            static const std::regex kBr(R"(^\s*br\s+i1\s+(\S+),)");
            std::smatch bm;
            if (std::regex_search(term->line, bm, kBr) && bm[1].str() == p.second->result)
                edits[term->index] = std::nullopt;   // folded into the trace replacement
        }
    }

    // --- the loop blocks go --------------------------------------------------
    if (q.hasLoop) {
        std::set<std::string> gone = q.loop.body;
        if (!preheader.empty()) gone.insert(preheader);
        for (const auto& lbl : gone) {
            const llm::Block* b = fn.FindBlock(lbl);
            if (!b) continue;
            if (b->index >= 0) edits[b->index] = std::nullopt;
            for (const auto& i : b->instrs) edits[i.index] = std::nullopt;
        }
    }

    // --- committed accessors become payload reads ---------------------------
    for (const auto& c : q.committedOps) {
        const llm::Instr& i = *c.second;
        const auto f = PayloadField(i.DxOp());
        if (!f) continue;
        if (i.DxOp() == kCommittedFrontFace) {
            const std::string tmp = "%rq.ff" + i.result.substr(1);
            edits[i.index] = "  " + tmp + " = load i32, i32* %rq.pl7, align 4\n  " +
                i.result + " = icmp eq i32 " + tmp + ", " +
                std::to_string(kHitKindFront);
        } else if (i.DxOp() == kCommittedWorldToObject) {
            static const std::regex kRowRe(R"(i32\s+(\d+))");
            static const std::regex kColRe(R"(i8\s+(\d+))");
            std::smatch rm2, cm2;
            std::string ar = Trim(i.args[2]), ac = Trim(i.args[3]);
            std::regex_search(ar, rm2, kRowRe);
            std::regex_search(ac, cm2, kColRe);
            const int slot = std::stoi(rm2[1].str()) * 4 + std::stoi(cm2[1].str());
            const std::string ptr = "%rq.w" + i.result.substr(1);
            edits[i.index] =
                "  " + ptr + " = getelementptr inbounds [12 x float], [12 x float]* "
                "%rq.pl8, i32 0, i32 " + std::to_string(slot) + "\n  " +
                i.result + " = load float, float* " + ptr + ", align 4";
        } else if (i.DxOp() == kCommittedBary) {
            static const std::regex kI8(R"(i8\s+(\d+))");
            std::smatch mm;
            std::string a2 = Trim(i.args[2]);
            std::regex_search(a2, mm, kI8);
            const std::string tmp = "%rq.bv" + i.result.substr(1);
            edits[i.index] = "  " + tmp +
                " = load <2 x float>, <2 x float>* %rq.pl1, align 4\n  " + i.result +
                " = extractelement <2 x float> " + tmp + ", i32 " + mm[1].str();
        } else {
            edits[i.index] = "  " + i.result + " = load " + f->ty + ", " + f->ty +
                "* %rq.pl" + std::to_string(f->idx) + ", align " +
                std::to_string(f->align);
        }
    }

    edits[fn.index] = "define void @" + e.raygen + "() #1 {";

    std::string out = llm::Render(m, edits);

    // --- types and globals --------------------------------------------------
    {
        std::vector<std::string> decls;
        decls.push_back(std::string(kPayload) + " = type " + kPayloadType);
        decls.push_back(std::string(kAttrs) + " = type { <2 x float> }");
        decls.push_back("");
        std::set<std::string> seen;
        for (const auto& g : globals) {
            if (!seen.insert(g.sym).second) continue;
            decls.push_back("@" + g.sym + " = external constant " + g.gty + ", align 4");
        }
        static const std::regex kAnchor(R"(%dx\.types\.Handle = type .*)");
        auto lines = llm::SplitLines(out);
        const int at = llm::FindLine(lines, kAnchor);
        if (at < 0) {
            r.error = "cannot find %dx.types.Handle to anchor declarations";
            return r;
        }
        std::string joined;
        for (size_t i = 0; i < decls.size(); ++i) {
            if (i) joined += "\n";
            joined += decls[i];
        }
        llm::InsertAfter(lines, at, joined);
        out = llm::JoinLines(lines);
    }

    // --- declarations --------------------------------------------------------
    {
        const char* kill[] = {
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.rayQuery_[^\n]*\n)",
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.allocateRayQuery[^\n]*\n)",
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.createHandle\(i32, i8[^\n]*\n)",
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.threadId[^\n]*\n)",
        };
        for (const char* k : kill) out = std::regex_replace(out, std::regex(k), "\n");

        std::vector<std::string> add = {
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #0",
            "", "; Function Attrs: nounwind",
            "declare void @dx.op.traceRay." + std::string(kPayload).substr(1) +
                "(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, "
                "float, float, float, float, float, " + kPayload + "*) #1",
            "", "; Function Attrs: nounwind readonly",
            "declare float @dx.op.rayTCurrent.f32(i32) #2",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.instanceIndex.i32(i32) #0",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.primitiveIndex.i32(i32) #0",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.instanceID.i32(i32) #0",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.hitKind.i32(i32) #0",
        };
        // An UNUSED declare is itself a validation error, so these three are
        // emitted only when something calls them. The rest are always used,
        // because the generated closest-hit reads them every time.
        std::set<int> usedOps;
        for (const auto& c : q.candidateOps) usedOps.insert(c.second->DxOp());
        for (const auto& c : q.committedOps) usedOps.insert(c.second->DxOp());
        auto addDecl = [&](bool want, const char* d) {
            if (!want) return;
            add.push_back("");
            add.push_back("; Function Attrs: nounwind readnone");
            add.push_back(d);
        };
        addDecl(usedOps.count(kCandidateWorldToObject) ||
                usedOps.count(kCommittedWorldToObject),
                "declare float @dx.op.worldToObject.f32(i32, i32, i8) #0");
        addDecl(usedOps.count(kCandidateObjectRayOrigin) != 0,
                "declare float @dx.op.objectRayOrigin.f32(i32, i8) #0");
        addDecl(usedOps.count(kCandidateObjectRayDirection) != 0,
                "declare float @dx.op.objectRayDirection.f32(i32, i8) #0");
        if (q.hasLoop) {
            add.push_back("");
            add.push_back("; Function Attrs: noreturn nounwind");
            add.push_back("declare void @dx.op.ignoreHit(i32) #3");
        }
        std::set<std::string> seenFn;
        for (const auto& g : globals) {
            const std::string f = HandleFn(g.elem);
            if (!seenFn.insert(f).second) continue;
            add.push_back("");
            add.push_back("; Function Attrs: nounwind readonly");
            add.push_back("declare %dx.types.Handle @" + f + "(i32, " + g.elem + ") #2");
        }
        const size_t anchor = out.find("\nattributes #0 =");
        std::string joined;
        for (size_t i = 0; i < add.size(); ++i) {
            if (i) joined += "\n";
            joined += add[i];
        }
        out = out.substr(0, anchor) + "\n" + joined + out.substr(anchor);
        if (q.hasLoop && out.find("attributes #3") == std::string::npos)
            out = Replace(out, "attributes #2 = { nounwind readonly }",
                          "attributes #2 = { nounwind readonly }\n"
                          "attributes #3 = { noreturn nounwind }");
    }

    // --- generated shaders ---------------------------------------------------
    {
        std::vector<std::string> fns;
        if (q.hasLoop) {
            // AnyHit IS the Proceed loop body, re-rooted. Accept is falling off
            // the end; reject is IgnoreHit. The polarity is the reverse of the
            // RayQuery form, where committing is the special path.
            std::set<std::string> commitBlocks;
            for (const auto& c : q.commits) commitBlocks.insert(c.first->label);

            std::map<std::string, std::string> subst;
            for (const auto& lbl : q.loop.body) {
                if (lbl == q.loop.latch) continue;
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                for (const auto& i : b->instrs)
                    if (i.DxOp() == kCandidateType) subst[i.result] = "0";
            }

            std::vector<std::string> order{ q.loop.header };
            {
                std::vector<std::string> rest;
                for (const auto& l : q.loop.body)
                    if (l != q.loop.header && l != q.loop.latch) rest.push_back(l);
                std::sort(rest.begin(), rest.end());
                for (const auto& l : rest) order.push_back(l);
            }

            std::vector<std::string> ah;
            ah.push_back("define void @" + e.anyhit + "(" + kPayload +
                         "* noalias nocapture %p, " + kAttrs +
                         "* nocapture readonly %attr) #1 {");
            ah.push_back(std::string("  %rq.ap = getelementptr inbounds ") + kAttrs +
                         ", " + kAttrs + "* %attr, i32 0, i32 0");
            ah.push_back("  %rq.ab = load <2 x float>, <2 x float>* %rq.ap, align 4");

            // Recreate every resource handle the body uses, under the SAME SSA
            // name it had in the raygen, so the transplanted instructions need
            // no rewriting.
            std::set<std::string> usedNames;
            for (const auto& lbl : q.loop.body) {
                if (lbl == q.loop.latch) continue;
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                for (const auto& i : b->instrs)
                    for (const auto& u : i.Uses()) usedNames.insert(u);
            }
            for (const auto& b : fn.blocks)
                for (const auto& i : b.instrs)
                    if (i.DxOp() == kCreateHandle && usedNames.count(i.result)) {
                        const std::string t = handleText(i);
                        if (!handleErr.empty()) { r.error = handleErr; return r; }
                        ah.push_back(t);
                    }

            for (const auto& lbl : order) {
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                if (lbl != q.loop.header) {
                    ah.push_back("");
                    ah.push_back(lbl.substr(1) + ":");
                }
                for (const auto& i : b->instrs) {
                    const int op = i.DxOp();
                    if (op == kCandidateType || op == kCommitNonOpaque) continue;
                    if (op == kCandidateFrontFace) {
                        // RayQuery returns a bool; HitKind() is an integer.
                        const std::string hk = "%rq.hk" + i.result.substr(1);
                        ah.push_back("  " + hk +
                                     " = call i32 @dx.op.hitKind.i32(i32 143)"
                                     "  ; HitKind()");
                        ah.push_back("  " + i.result + " = icmp eq i32 " + hk + ", " +
                                     std::to_string(kHitKindFront));
                        continue;
                    }
                    if (const CandMap* cmap = FindCand(op)) {
                        // Same operands as the RayQuery form, minus the handle.
                        std::string cargs = "i32 " + std::to_string(cmap->dxop);
                        for (int k = 0; k < cmap->extra; ++k)
                            cargs += ", " + Trim(i.args[2 + k]);
                        ah.push_back("  " + i.result + " = call " + cmap->ty + " @" +
                                     cmap->callee + "(" + cargs + ")");
                        continue;
                    }
                    if (op == kCandidateBary) {
                        static const std::regex kI8(R"(i8\s+(\d+))");
                        std::smatch mm;
                        std::string a2 = Trim(i.args[2]);
                        std::regex_search(a2, mm, kI8);
                        ah.push_back("  " + i.result +
                                     " = extractelement <2 x float> %rq.ab, i32 " +
                                     mm[1].str());
                        continue;
                    }
                    std::string line = i.line;
                    for (const auto& kv : subst)
                        line = std::regex_replace(
                            line, std::regex(kv.first.substr(0, 1) == "%"
                                             ? "\\" + kv.first + "\\b" : kv.first),
                            kv.second);
                    const std::string target =
                        commitBlocks.count(lbl) ? "%rq.accept" : "%rq.reject";
                    line = std::regex_replace(
                        line, std::regex("label\\s+" + std::string("\\") + q.loop.latch +
                                         "\\b"),
                        "label " + target);
                    ah.push_back(line);
                }
            }
            ah.push_back("");
            ah.push_back("rq.accept:");
            ah.push_back("  ret void");
            ah.push_back("");
            ah.push_back("rq.reject:");
            ah.push_back("  call void @dx.op.ignoreHit(i32 155)  ; IgnoreHit()");
            ah.push_back("  unreachable");
            ah.push_back("}");
            std::string joined;
            for (size_t i = 0; i < ah.size(); ++i) {
                if (i) joined += "\n";
                joined += ah[i];
            }
            fns.push_back(joined);
        }

        // ClosestHit
        {
            std::vector<std::string> ch;
            ch.push_back("define void @" + e.closesthit + "(" + kPayload +
                         "* noalias nocapture %p, " + kAttrs +
                         "* nocapture readonly %attr) #1 {");
            ch.push_back("  %t = call float @dx.op.rayTCurrent.f32(i32 154)  ; RayTCurrent()");
            ch.push_back(std::string("  %ap = getelementptr inbounds ") + kAttrs + ", " +
                         kAttrs + "* %attr, i32 0, i32 0");
            ch.push_back("  %b = load <2 x float>, <2 x float>* %ap, align 4");
            ch.push_back(std::string("  %pt = getelementptr inbounds ") + kPayload + ", " +
                         kPayload + "* %p, i32 0, i32 0");
            ch.push_back("  store float %t, float* %pt, align 4");
            ch.push_back(std::string("  %pb = getelementptr inbounds ") + kPayload + ", " +
                         kPayload + "* %p, i32 0, i32 1");
            ch.push_back("  store <2 x float> %b, <2 x float>* %pb, align 4");
            ch.push_back(std::string("  %ph = getelementptr inbounds ") + kPayload + ", " +
                         kPayload + "* %p, i32 0, i32 2");
            ch.push_back("  store i32 1, i32* %ph, align 4");
            for (const auto& s : kChSource) {
                ch.push_back("  %id" + std::to_string(s.first) + " = " + s.second);
                ch.push_back("  %pi" + std::to_string(s.first) +
                             " = getelementptr inbounds " + kPayload + ", " + kPayload +
                             "* %p, i32 0, i32 " + std::to_string(s.first));
                ch.push_back("  store i32 %id" + std::to_string(s.first) + ", i32* %pi" +
                             std::to_string(s.first) + ", align 4");
            }
            // Twelve fetches and twelve stores, so they are emitted ONLY when
            // the shader actually reads the matrix. The payload field exists
            // either way, because a layout that changes shape is a layout that
            // gets an offset wrong; it is the per-invocation work worth
            // avoiding.
            bool wantsW2O = false;
            for (const auto& cw : q.committedOps)
                if (cw.second->DxOp() == kCommittedWorldToObject) wantsW2O = true;
            if (wantsW2O) {
                ch.push_back(std::string("  %pw = getelementptr inbounds ") + kPayload +
                             ", " + kPayload + "* %p, i32 0, i32 8");
                for (int rr = 0; rr < 3; ++rr) {
                    for (int cc = 0; cc < 4; ++cc) {
                        const int sl = rr * 4 + cc;
                        ch.push_back("  %w" + std::to_string(sl) +
                                     " = call float @dx.op.worldToObject.f32(i32 152, i32 " +
                                     std::to_string(rr) + ", i8 " + std::to_string(cc) +
                                     ")  ; WorldToObject(row,col)");
                        ch.push_back("  %pw" + std::to_string(sl) +
                                     " = getelementptr inbounds [12 x float], "
                                     "[12 x float]* %pw, i32 0, i32 " + std::to_string(sl));
                        ch.push_back("  store float %w" + std::to_string(sl) +
                                     ", float* %pw" + std::to_string(sl) + ", align 4");
                    }
                }
            }
            ch.push_back("  ret void");
            ch.push_back("}");
            std::string joined;
            for (size_t i = 0; i < ch.size(); ++i) {
                if (i) joined += "\n";
                joined += ch[i];
            }
            fns.push_back(joined);
        }

        // Miss
        fns.push_back("define void @" + e.miss + "(" + std::string(kPayload) +
                      "* noalias nocapture %p) #1 {\n"
                      "  %ph = getelementptr inbounds " + kPayload + ", " + kPayload +
                      "* %p, i32 0, i32 2\n"
                      "  store i32 0, i32* %ph, align 4\n"
                      "  ret void\n}");

        static const std::regex kEnd(R"(\}\s*)");
        auto lines = llm::SplitLines(out);
        const int at = llm::FindLine(lines, kEnd);
        if (at < 0) {
            r.error = "cannot find the end of the entry function";
            return r;
        }
        std::string joined;
        for (size_t i = 0; i < fns.size(); ++i) {
            if (i) joined += "\n\n";
            joined += fns[i];
        }
        llm::InsertAfter(lines, at, "\n" + joined);
        out = llm::JoinLines(lines);
    }

    // --- metadata ------------------------------------------------------------
    {
        for (const auto& g : globals) {
            auto fields = SplitTop(md.at(g.nid));
            fields[1] = std::regex_replace(fields[1], std::regex(R"(\*\s+\S+$)"),
                                           "* @" + g.sym);
            fields[2] = "!\"" + g.name + "\"";
            std::string body;
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i) body += ", ";
                body += fields[i];
            }
            {
                const std::regex re("!" + std::to_string(g.nid) + R"( = !\{.*\}\s*)");
                auto lines = llm::SplitLines(out);
                const int at = llm::FindLine(lines, re);
                if (at >= 0)
                    lines[at] = "!" + std::to_string(g.nid) + " = !{" + body + "}";
                out = llm::JoinLines(lines);
            }
        }

        {
            static const std::regex re(
                R"RX(!(\d+) = !\{!"cs", i32 (\d+), i32 (\d+)\}\s*)RX");
            auto lines = llm::SplitLines(out);
            const int at = llm::FindLine(lines, re);
            if (at >= 0) {
                std::smatch cm;
                std::regex_match(lines[at], cm, re);
                lines[at] = "!" + cm[1].str() + " = !{!\"lib\", i32 " + cm[2].str() +
                            ", i32 " + cm[3].str() + "}";
            }
            out = llm::JoinLines(lines);
        }

        std::smatch epm;
        std::regex_search(out, epm, std::regex(R"(!dx\.entryPoints\s*=\s*!\{!(\d+)\})"));
        const std::string oldEntry = epm[1].str();

        int counter = 0;
        for (const auto& kv : md) counter = std::max(counter, kv.first);
        ++counter;
        std::vector<std::string> nodes;
        auto node = [&](const std::string& body) {
            const int id = counter++;
            nodes.push_back("!" + std::to_string(id) + " = !{" + body + "}");
            return id;
        };

        const std::string sigH = "(" + std::string(kPayload) + "*, " + kAttrs + "*)";
        const std::string sigP = "(" + std::string(kPayload) + "*)";

        const int empty = node("");
        const int ret = node("i32 1, !" + std::to_string(empty) + ", !" + std::to_string(empty));
        const int parPayload = node("i32 2, !" + std::to_string(empty) + ", !" + std::to_string(empty));
        const int parAttrs = node("i32 0, !" + std::to_string(empty) + ", !" + std::to_string(empty));
        const int annRaygen = node("!" + std::to_string(ret));
        const int annHit = node("!" + std::to_string(ret) + ", !" + std::to_string(parPayload) +
                                ", !" + std::to_string(parAttrs));
        const int annMiss = node("!" + std::to_string(ret) + ", !" + std::to_string(parPayload));

        std::vector<std::string> ann{ "i32 1", "void ()* @" + e.raygen,
                                      "!" + std::to_string(annRaygen) };
        if (q.hasLoop) {
            ann.push_back("void " + sigH + "* @" + e.anyhit);
            ann.push_back("!" + std::to_string(annHit));
        }
        ann.push_back("void " + sigH + "* @" + e.closesthit);
        ann.push_back("!" + std::to_string(annHit));
        ann.push_back("void " + sigP + "* @" + e.miss);
        ann.push_back("!" + std::to_string(annMiss));
        std::string annJoined;
        for (size_t i = 0; i < ann.size(); ++i) {
            if (i) annJoined += ", ";
            annJoined += ann[i];
        }
        const int annId = node(annJoined);

        const int zero = node("i32 0");
        const int flags = node("i32 0, i64 16");
        std::vector<int> eps{ node("null, !\"\", null, !" + resId + ", !" +
                                   std::to_string(flags)) };

        auto entry = [&](const std::string& name, const std::string& sig, int kind,
                         bool payload, bool attrs) {
            std::vector<std::string> props{ "i32 8", "i32 " + std::to_string(kind) };
            if (payload) { props.push_back("i32 6"); props.push_back("i32 " + std::to_string(kPayloadBytes)); }
            if (attrs) { props.push_back("i32 7"); props.push_back("i32 8"); }
            props.push_back("i32 5");
            props.push_back("!" + std::to_string(zero));
            std::string pj;
            for (size_t i = 0; i < props.size(); ++i) {
                if (i) pj += ", ";
                pj += props[i];
            }
            const int pid = node(pj);
            return node("void " + sig + "* @" + name + ", !\"" + name +
                        "\", null, null, !" + std::to_string(pid));
        };

        if (q.hasLoop) eps.push_back(entry(e.anyhit, sigH, 9, true, true));
        eps.push_back(entry(e.closesthit, sigH, 10, true, true));
        eps.push_back(entry(e.miss, sigP, 11, true, false));
        eps.push_back(entry(e.raygen, "()", 7, false, false));

        std::string epJoined;
        for (size_t i = 0; i < eps.size(); ++i) {
            if (i) epJoined += ", ";
            epJoined += "!" + std::to_string(eps[i]);
        }
        out = Replace(out, "!dx.entryPoints = !{!" + oldEntry + "}",
                      "!dx.typeAnnotations = !{!" + std::to_string(annId) + "}\n"
                      "!dx.entryPoints = !{" + epJoined + "}");
        {
            const std::regex re("!" + oldEntry +
                                R"( = !\{void \(\)\* @\w+.*\}\s*)");
            auto lines = llm::SplitLines(out);
            const int at = llm::FindLine(lines, re);
            if (at >= 0) lines[at] = "";
            out = llm::JoinLines(lines);
        }

        while (!out.empty() && out.back() == '\n') out.pop_back();
        std::string nodeJoined;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (i) nodeJoined += "\n";
            nodeJoined += nodes[i];
        }
        out += "\n" + nodeJoined + "\n";
    }

    r.ok = true;
    r.text = out;
    return r;
}

}  // namespace rq
