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
//   7 hitKind | 8 worldToObject, 12 floats | 9 aborted
//
// The LAYOUT is fixed even though the matrix is only sometimes read: variable
// offsets across two implementations is a good way to get one of them subtly
// wrong. The per-invocation COST is what is made conditional instead.
const char* kPayloadType =
    "{ float, <2 x float>, i32, i32, i32, i32, i32, i32, [12 x float], i32, i32 }";
const int kPayloadBytes = 92;

// The record's own two numbers, and how the generated shaders reach them.
//
// Not a dx.op call like everything in kChSource: there is no intrinsic for
// either, which is exactly why they were refused. They come out of a cbuffer
// bound by a LOCAL root signature, so the value is per hit-group-record and
// the shim chose it when it built the table.
const char* kRecordType = "%rq_record";
const char* kRecordGlobal = "@rq_record";
const char* kRecordHandleFn = "dx.op.createHandleForLib.rq_record";
const char* kCbRet = "%dx.types.CBufRet.i32";
// HitKind() is an integer; the RayQuery form is a bool.
const int kHitKindFront = 254;

struct Field { int idx; const char* ty; int align; };

// Emitted per use rather than hoisted. The handle and the load are pure and
// the driver folds the duplicates; hoisting them by hand would mean finding a
// place that dominates every use, which is a CFG question this pass has no
// reason to ask.
std::vector<std::string> RecordRead(const std::string& tag, int word,
                                    const std::string& result) {
    const std::string t = std::string(kRecordType);
    return {
        "  %rq.cbv" + tag + " = load " + t + ", " + t + "* " + kRecordGlobal +
            ", align 4",
        "  %rq.cbh" + tag + " = call %dx.types.Handle @" + kRecordHandleFn +
            "(i32 160, " + t + " %rq.cbv" + tag + ")  ; CreateHandleForLib(Resource)",
        "  %rq.cbr" + tag + " = call " + kCbRet +
            " @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %rq.cbh" + tag +
            ", i32 0)  ; CBufferLoadLegacy(handle,regIndex)",
        "  " + result + " = extractvalue " + kCbRet + " %rq.cbr" + tag + ", " +
            std::to_string(word),
    };
}

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
        // 9 is the abort flag. 10 came with the record constants; the geometry
        // index needed no new field because slot 5 had been reserved for it all
        // along and left unused while the accessor was refused.
        case kCommittedGeometryIndex:   return Field{ 5, "i32", 4 };
        case kCommittedInstanceContrib: return Field{ 10, "i32", 4 };
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
    // Legal in an any-hit and an intersection shader, NOT in a raygen.
    // A raygen read is folded to a constant instead, see EditRayFlags.
    { kRayFlags,                    "i32",   "dx.op.rayFlags.i32",           144, 0 },
};
const CandMap* FindCand(int op) {
    for (const auto& c : kCandidateMap) if (c.op == op) return &c;
    return nullptr;
}

// Shader Model 6.6 reaches a resource by BINDING, not by a range index.
//
// Unreal's RayQuery shaders are cs_6_6 and every one of them does this. The
// rewriter was built against 6.5, and that single difference caused 124 of the
// 157 refusals a real Unreal run produced.
//
//   cs_6_6   createHandleFromBinding (217) -> annotateHandle (216)
//   lib_6_6  createHandleForLib      (160) -> annotateHandle (216)
//
// So the conversion is 217 -> 160, and the annotateHandle after it is kept
// untouched. Read off DXC, see phase5/cases/reference/lib_sm66_binding_ref.hlsl.
const int kBindHandle = 217;
const int kAnnotateHandle = 216;
const int kHeapHandle = 218;
// The library form's global is a HANDLE, not the resource type, and the
// overload is named after the handle type too. That is the part that cannot be
// guessed from the 6.5 path, where both are the resource type.
const char kHandleType[] = "%dx.types.Handle";

// 0x2000000 is the raytracing tier 1.1 shader flag: measured on GeometryIndex,
// which sets it and makes CreateStateObject refuse the library on a GTX 1070.
// Every RayQuery op is gone after lowering, so the flag must go with them.
const unsigned long long kRt11ShaderFlag = 0x2000000ull;

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
    // `distinct` counts. A [branch] or [loop] hint makes DXC emit
    // `!16 = distinct !{!16, !"dx.controlflow.hints", i32 1}`, and a
    // pattern that misses it leaves that id invisible. The fresh-id
    // counter starts at max+1, so it then hands out an id the module
    // already uses and the assembler says "Metadata id is already used".
    // Unreal uses those hints constantly.
    static const std::regex re(R"(!(\d+) = (?:distinct )?!\{(.*)\}\s*)");
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

// Every block this line branches to.
std::vector<std::string> Block_successors(const std::string& line) {
    static const std::regex re(R"(label\s+(%[\w.$-]+))");
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(line.begin(), line.end(), re);
         it != std::sregex_iterator(); ++it)
        out.push_back((*it)[1].str());
    return out;
}

std::string Replace(std::string s, const std::string& from, const std::string& to) {
    size_t p = s.find(from);
    if (p != std::string::npos) s.replace(p, from.size(), to);
    return s;
}


// --- what a generated hit shader can rebuild for itself ---------------------
//
// Mirrors _type_names / _operands / _recomputable / _needed_chain in lower.py,
// which is the reference. See there for why each list is what it is.

// Every %name an instruction READS, not just its call arguments.
//
// Instr::Uses() decodes call arguments and phi incomings, which is what the
// rest of this pass needs. It is not enough for the isolation check: a value
// can reach the loop body through ordinary arithmetic, `fcmp float %bary,
// %thresh`, and Uses() cannot see it. The check then let it through, the
// generated hit shader referenced a value it never defined, and the ASSEMBLER
// reported it, as "use of undefined value". Loud, but nowhere near the cause.
std::set<std::string> TypeNames(const std::string& text) {
    // A name is a TYPE exactly when the module declares it one. Guessing from
    // the shape of the name does not work: %rq.pl and %dx.types.Handle look
    // alike.
    static const std::regex kTypedef(R"RX(^(%"[^"]*"|%[\w.$-]+)\s*=\s*type\s)RX");
    std::set<std::string> out;
    for (const auto& line : llm::SplitLines(text)) {
        std::smatch m;
        if (std::regex_search(line, m, kTypedef)) out.insert(m[1].str());
    }
    return out;
}

std::vector<std::string> Operands(const llm::Instr& i,
                                  const std::set<std::string>& types) {
    // A phi writes its predecessors as `[ %val, %bb12 ]`, with no `label`
    // keyword to mark them, so the general scan below counts %bb12 as a value
    // read. Unreal's loop bodies are full of phis, and the check then refused
    // 118 shaders for "reading" their own predecessor blocks.
    //
    // Uses() already decodes a phi correctly: incoming VALUES, which do count,
    // and not the blocks, which do not.
    if (i.isPhi) return i.Uses();

    std::string body = i.body;
    const size_t cut = body.find(';');
    if (cut != std::string::npos) body = body.substr(0, cut);

    // A quoted name first, because %"class.RWStructuredBuffer<Result>" holds
    // characters an unquoted one cannot and would be cut at the quote.
    static const std::regex kName(R"RX(%"[^"]*"|%[\w.$-]+)RX");
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), kName);
         it != std::sregex_iterator(); ++it) {
        const std::string n = it->str();
        if (types.count(n) || n == i.result) continue;
        // `label %bb42` is a branch target, not a value.
        std::string before = body.substr(0, it->position());
        while (!before.empty() && std::isspace((unsigned char)before.back()))
            before.pop_back();
        if (before.size() >= 5 && before.compare(before.size() - 5, 5, "label") == 0)
            continue;
        out.push_back(n);
    }
    return out;
}

bool PureInstruction(const llm::Instr& i) {
    static const std::set<std::string> kPure = {
        "add", "sub", "mul", "and", "or", "xor", "shl", "lshr", "ashr",
        "fadd", "fsub", "fmul", "fdiv", "fneg",
        "icmp", "fcmp", "select", "extractvalue", "extractelement",
        "zext", "sext", "trunc", "bitcast", "sitofp", "uitofp", "fptosi",
        "fptoui", "fpext", "fptrunc", "getelementptr",
    };
    static const std::regex kOp(R"(^\s*(\w+))");
    std::smatch m;
    if (!std::regex_search(i.body, m, kOp)) return false;
    return kPure.count(m[1].str()) != 0;
}

// 57 createHandle, 59 cbufferLoadLegacy, 93 threadId, plus the Shader Model
// 6.6 handle pair 216/217 and the bindless form 218. See lower.py for what is
// deliberately absent: loads, phis, integer division, any other dx.op.
//
// 218 was left off deliberately and wrongly. The 0.25.0 note said a heap
// handle used inside the Proceed loop is refused because 218 is not on this
// list, and filed that as correct; it was measured on a shader that kept all
// 16 of its heap handles in the raygen, so nothing counted the in-loop case.
// Counted: 58 of the 157 refusals from one Escher run are exactly this.
//
// It is recomputable for the reason the other handles are, one step further
// out. The descriptor heap is set on the command list and is the same heap in
// the any-hit as in the raygen, and the index reaches it from a cbuffer, which
// holds the same bytes for the whole dispatch. The fixpoint is what enforces
// "from a cbuffer": 218 is admitted only when its index is ALREADY
// recomputable, so an index built from a UAV read or a phi still refuses. It
// needs no conversion either, unlike 57 and 217, because a heap handle means
// the same thing in a library, so it is emitted verbatim.
//
// 94, 95 and 96 are the group forms of 93, and the hit shader rebuilds them
// from DispatchRaysIndex exactly as the raygen does. See ThreadIndexText.
bool PureDxOp(int op) {
    return op == 57 || op == 59 || op == 93 ||
           op == kGroupId || op == kThreadIdInGroup || op == kFlatThreadIdInGroup ||
           op == kAnnotateHandle || op == kBindHandle || op == kHeapHandle;
}

// The compute thread-index ops. None is legal in a ray tracing stage, and a
// raygen has no thread group at all. But the shim launches exactly
// groups * numthreads rays, one per thread, so DispatchRaysIndex IS the
// thread's SV_DispatchThreadID and the other three follow from it:
//
//   93  SV_DispatchThreadID.c  = DispatchRaysIndex.c
//   94  SV_GroupID.c           = DispatchRaysIndex.c / numthreads.c
//   95  SV_GroupThreadID.c     = DispatchRaysIndex.c % numthreads.c
//   96  SV_GroupIndex          = (gt.z * ny + gt.y) * nx + gt.x, gt the above
//
// Exact. What a group actually SHARES, groupshared and barriers, is refused by
// the analysis. The divisors are constants of at least 1, so nothing hoisted
// can divide by zero. The Python original is _thread_index_text in lower.py.
bool IsThreadOp(int op) {
    return op == kThreadId || op == kGroupId || op == kThreadIdInGroup ||
           op == kFlatThreadIdInGroup;
}

const char* kNoNumThreads =
    "the entry point declares no numthreads, so the thread group index cannot "
    "be rebuilt";

std::string ThreadIndexText(const llm::Instr& i, const llm::Module& m,
                            std::string* err) {
    const std::string& r = i.result;
    const std::string tag = r.substr(1);
    auto Dri = [](const std::string& name, int comp) {
        return "  " + name + " = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 " +
               std::to_string(comp) + ")  ; DispatchRaysIndex(col)";
    };
    const int op = i.DxOp();
    int nt[3] = { 1, 1, 1 };

    if (op == kThreadId || op == kGroupId || op == kThreadIdInGroup) {
        static const std::regex kCol(R"RX(^i32\s+(\d+))RX");
        std::smatch cm;
        const std::string a1 = i.args.size() > 1 ? Trim(i.args[1]) : std::string();
        const int comp = std::regex_search(a1, cm, kCol) ? std::stoi(cm[1].str()) : 0;
        if (comp > 2) {
            *err = "thread index component " + std::to_string(comp) + " out of range";
            return std::string();
        }
        if (op == kThreadId) return Dri(r, comp);
        if (!NumThreads(m, nt)) { *err = kNoNumThreads; return std::string(); }
        const std::string d = "%rq.dri." + tag;
        return Dri(d, comp) + "\n  " + r + " = " +
               (op == kGroupId ? "udiv" : "urem") + " i32 " + d + ", " +
               std::to_string(nt[comp]);
    }

    if (!NumThreads(m, nt)) { *err = kNoNumThreads; return std::string(); }
    std::string out;
    const char* axes = "xyz";
    for (int c = 0; c < 3; ++c) {
        const std::string ax(1, axes[c]);
        out += Dri("%rq.dri." + tag + "." + ax, c) + "\n";
        out += "  %rq.gt." + tag + "." + ax + " = urem i32 %rq.dri." + tag + "." + ax +
               ", " + std::to_string(nt[c]) + "\n";
    }
    out += "  %rq.gi." + tag + ".a = mul i32 %rq.gt." + tag + ".z, " +
           std::to_string(nt[1]) + "\n";
    out += "  %rq.gi." + tag + ".b = add i32 %rq.gi." + tag + ".a, %rq.gt." + tag + ".y\n";
    out += "  %rq.gi." + tag + ".c = mul i32 %rq.gi." + tag + ".b, " +
           std::to_string(nt[0]) + "\n";
    out += "  " + r + " = add i32 %rq.gi." + tag + ".c, %rq.gt." + tag + ".x";
    return out;
}

std::map<std::string, const llm::Instr*> Recomputable(
        const llm::Function& fn, const std::set<std::string>& types) {
    std::map<std::string, const llm::Instr*> ok;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& b : fn.blocks) {
            for (const auto& i : b.instrs) {
                if (i.result.empty() || ok.count(i.result)) continue;
                const int op = i.DxOp();
                if (op >= 0) { if (!PureDxOp(op)) continue; }
                else if (!PureInstruction(i)) continue;
                bool all = true;
                for (const auto& u : Operands(i, types))
                    if (!ok.count(u) && u != i.result) { all = false; break; }
                if (!all) continue;
                ok[i.result] = &i;
                changed = true;
            }
        }
    }
    return ok;
}

// `skip` is what the loop body defines for itself. Without it the closure walks
// straight back into the body and rebuilds its instructions in the prologue as
// well, which is a duplicate definition.
std::vector<const llm::Instr*> NeededChain(
        const llm::Function& fn,
        const std::map<std::string, const llm::Instr*>& rec,
        const std::set<std::string>& wanted,
        const std::set<std::string>& types,
        const std::set<std::string>& skip) {
    std::set<std::string> need;
    std::vector<std::string> frontier;
    for (const auto& w : wanted) if (!skip.count(w)) frontier.push_back(w);
    while (!frontier.empty()) {
        const std::string r = frontier.back();
        frontier.pop_back();
        if (need.count(r) || skip.count(r)) continue;
        auto it = rec.find(r);
        if (it == rec.end()) continue;
        need.insert(r);
        for (const auto& u : Operands(*it->second, types)) frontier.push_back(u);
    }
    std::vector<const llm::Instr*> out;
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (need.count(i.result)) out.push_back(&i);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

bool UsesBinding(const llm::Module& m) {
    if (m.functions.empty()) return false;
    for (const auto& b : m.functions[0].blocks)
        for (const auto& i : b.instrs)
            if (i.DxOp() == kBindHandle) return true;
    return false;
}

struct Bind {
    int cls = 0;
    int space = 0;
    int lower = 0;
    bool operator<(const Bind& o) const {
        if (cls != o.cls) return cls < o.cls;
        if (space != o.space) return space < o.space;
        return lower < o.lower;
    }
};

// `%dx.types.ResBind { i32 lower, i32 upper, i32 space, i8 class }`, read off
// real DXC output: `{ i32 4, i32 4, i32 0, i8 2 }` is cbuffer b4, space 0.
bool ResBind(const llm::Instr& i, Bind* out) {
    static const std::regex re(
        R"(\{\s*i32\s+(-?\d+),\s*i32\s+(-?\d+),\s*i32\s+(-?\d+),\s*i8\s+(-?\d+)\s*\})");
    if (i.args.size() < 2) return false;
    // SRV t0 space0 is all zeroes, and LLVM prints an all-zero struct as
    // `zeroinitializer` rather than writing the fields out. Nothing in the
    // Unreal shaders was bound there, so only an independently written case
    // reached this.
    if (i.args[1].find("zeroinitializer") != std::string::npos) {
        out->cls = 0; out->space = 0; out->lower = 0;
        return true;
    }
    std::smatch m;
    if (!std::regex_search(i.args[1], m, re)) return false;
    out->lower = std::stoi(m[1].str());
    out->space = std::stoi(m[3].str());
    out->cls = std::stoi(m[4].str());
    return true;
}

// {class, space, lowerBound} -> record node id. A record is
// `{id, global, name, space, lowerBound, rangeSize, ...}`, so the binding in
// the call is matched by space and lower bound rather than by the range index
// the 6.5 form carries.
std::map<Bind, int> BindingMap(const std::string& text,
                               const std::map<int, std::string>& md) {
    std::map<Bind, int> out;
    static const std::regex kRes(R"(!dx\.resources\s*=\s*!\{!(\d+)\})");
    std::smatch rm;
    if (!std::regex_search(text, rm, kRes)) return out;
    auto groups = SplitTop(md.at(std::stoi(rm[1].str())));
    static const std::regex kI32(R"(i32\s+(-?\d+))");
    for (size_t cls = 0; cls < groups.size(); ++cls) {
        const std::string g = Trim(groups[cls]);
        if (g == "null") continue;
        for (const auto& idTok : SplitTop(md.at(std::stoi(g.substr(1))))) {
            const int nid = std::stoi(Trim(idTok).substr(1));
            auto f = SplitTop(md.at(nid));
            if (f.size() < 5) continue;
            std::smatch sm, lm;
            const std::string sp = Trim(f[3]), lo = Trim(f[4]);
            if (!std::regex_search(sp, sm, kI32)) continue;
            if (!std::regex_search(lo, lm, kI32)) continue;
            Bind b;
            b.cls = static_cast<int>(cls);
            b.space = std::stoi(sm[1].str());
            b.lower = std::stoi(lm[1].str());
            out[b] = nid;
        }
    }
    return out;
}

// The module-level shader flags, tag 0 of the entry point's properties.
//
// The lowering used to hardcode 16 here, which happened to be right for every
// shader this project had ever seen: they all declared 0x2000010, and the only
// bit the lowering removes is the tier 1.1 flag. An Unreal shader declares
// 0x42000010, so 16 was wrong by exactly the bit it did not know about and the
// validator said "Flags must match usage".
// Attribute group NUMBERS are a fact about the input module, not a constant.
// See lower.py for what Unreal's numbering did to AnyHitNull and why the
// message pointed at the `unreachable` rather than at the missing noreturn.
struct AttrPlaceholder { const char* tag; const char* body; };
const AttrPlaceholder kAttrPlaceholders[] = {
    { "#RQNONE", "nounwind readnone" },
    { "#RQNW",   "nounwind" },
    { "#RQRO",   "nounwind readonly" },
    { "#RQNR",   "noreturn nounwind" },
};

std::string ResolveAttrs(const std::string& in, std::string* err) {
    auto lines = llm::SplitLines(in);
    static const std::regex kAttr(R"(attributes #(\d+) = \{ (.*) \}\s*)");
    std::map<std::string, int> have;
    std::set<int> used;
    int last = -1;
    for (size_t n = 0; n < lines.size(); ++n) {
        std::smatch m;
        if (!std::regex_match(lines[n], m, kAttr)) continue;
        const int id = std::stoi(m[1].str());
        used.insert(id);
        // First wins, matching the Python.
        have.emplace(Trim(m[2].str()), id);
        last = static_cast<int>(n);
    }
    std::vector<AttrPlaceholder> wanted;
    for (const auto& a : kAttrPlaceholders)
        if (in.find(a.tag) != std::string::npos) wanted.push_back(a);

    std::vector<std::string> added;
    for (const auto& a : wanted) {
        if (have.count(a.body)) continue;
        int id = 0;
        while (used.count(id)) ++id;
        used.insert(id);
        have[a.body] = id;
        added.push_back("attributes #" + std::to_string(id) + " = { " + a.body + " }");
    }
    std::string out = in;
    if (!added.empty()) {
        if (last < 0) {
            *err = "module declares no attribute groups";
            return in;
        }
        lines.insert(lines.begin() + last + 1, added.begin(), added.end());
        out = llm::JoinLines(lines);
    }
    // Replace ALL of them. The Replace helper here does the first occurrence
    // only, where Python's str.replace does every one, and a placeholder
    // appears once per generated function.
    for (const auto& a : wanted) {
        const std::string tag = a.tag;
        const std::string num = "#" + std::to_string(have[a.body]);
        for (size_t p = out.find(tag); p != std::string::npos;
             p = out.find(tag, p + num.size()))
            out.replace(p, tag.size(), num);
    }
    return out;
}

unsigned long long ModuleFlags(const std::string& text,
                               const std::map<int, std::string>& md) {
    static const std::regex kEp(R"(!dx\.entryPoints\s*=\s*!\{!(\d+)\})");
    std::smatch m;
    if (!std::regex_search(text, m, kEp)) return 0;
    auto it = md.find(std::stoi(m[1].str()));
    if (it == md.end()) return 0;
    auto fields = SplitTop(it->second);
    if (fields.size() < 5) return 0;
    const std::string p4 = Trim(fields[4]);
    if (p4.empty() || p4[0] != '!') return 0;
    auto pit = md.find(std::stoi(p4.substr(1)));
    if (pit == md.end()) return 0;
    auto props = SplitTop(pit->second);
    static const std::regex kI64(R"(^i64\s+(\d+))");
    for (size_t i = 0; i + 1 < props.size(); i += 2) {
        if (Trim(props[i]) != "i32 0") continue;
        std::smatch vm;
        const std::string v = Trim(props[i + 1]);
        if (std::regex_search(v, vm, kI64)) return std::stoull(vm[1].str());
    }
    return 0;
}

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

    const bool binding = UsesBinding(m);
    const std::map<Bind, int> binds = binding ? BindingMap(text0, md)
                                              : std::map<Bind, int>();

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

        if (instr.DxOp() == kBindHandle) {
            // Shader Model 6.6. The binding names the resource directly, so
            // the record is found by space and lower bound rather than by a
            // range index. The annotateHandle that follows is left exactly as
            // it was: it is legal in a library and carries the properties.
            Bind b;
            if (!ResBind(instr, &b)) {
                handleErr = "cannot read the ResBind of " + instr.body.substr(0, 60);
                return "";
            }
            auto bit = binds.find(b);
            if (bit == binds.end()) {
                handleErr = "createHandleFromBinding names class " +
                            std::to_string(b.cls) + " space " +
                            std::to_string(b.space) + " register " +
                            std::to_string(b.lower) +
                            ", which !dx.resources does not describe";
                return "";
            }
            const GlobalInfo* gb = findGlobal(b.cls, bit->second);
            if (!gb) {
                handleErr = "createHandleFromBinding names a resource with no global";
                return "";
            }
            const std::string a2b = instr.args.size() > 2 ? Trim(instr.args[2])
                                                          : std::string();
            if (!std::regex_match(a2b, kI32End)) {
                // The 6.5 path DOES support this, through a getelementptr on
                // the array global. The 6.6 form is refused only because the
                // shape of an ARRAY global in the binding form has not been
                // measured off DXC, and guessing it is how a lowering goes
                // silently wrong. phase5/cases/reference/ is where that
                // question gets answered.
                handleErr = "resource handle " + instr.result +
                            " comes from an array binding indexed dynamically (" +
                            a2b + "); the Shader Model 6.6 form of that has not "
                            "been measured, and the 6.5 form is what this lowers";
                return "";
            }
            const std::string rawb = "%rq.raw." + instr.result.substr(1);
            const std::string h = kHandleType;
            return "  " + rawb + " = load " + h + ", " + h + "* @" + gb->sym +
                   ", align 4\n"
                   "  " + instr.result + " = call " + h +
                   " @dx.op.createHandleForLib.dx.types.Handle(i32 160, " + h +
                   " " + rawb + ")  ; CreateHandleForLib(Resource)";
        }

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
        std::string pre;
        if (g->gty == g->elem) {
            src = g->elem + "* @" + g->sym;
        } else {
            // A resource array. DXC reaches the element through a
            // getelementptr on the array global and hands createHandleForLib
            // the ELEMENT type. Matched against a DXC-built library, see
            // phase5/cases/reference/lib_array_ref.hlsl for the constant form
            // and lib_dynarray_ref.hlsl for the dynamic one.
            const std::string a3 = Trim(instr.args[3]);
            const std::string nonuni = Trim(instr.args[4]);
            const bool nu = (nonuni != "i1 false" && nonuni != "i1 0");
            std::smatch im;
            if (std::regex_match(a3, im, kI32End)) {
                // A constant index folds into a constant getelementptr
                // EXPRESSION, inline in the call. Nothing runs at runtime.
                src = g->elem + "* getelementptr inbounds (" + g->gty + ", " +
                      g->gty + "* @" + g->sym + ", i32 0, i32 " + im[1].str() + ")";
            } else {
                // A dynamic index cannot be a constant expression, so it
                // becomes a real getelementptr INSTRUCTION on the same global,
                // with the index operand carried across untouched.
                const std::string gep = "%rq.gep." + instr.result.substr(1);
                // NonUniformResourceIndex shows up as !dx.nonuniform on the
                // getelementptr, and dropping it would be a silently wrong
                // lowering rather than a missing feature. The node id is not
                // known until the metadata is rebuilt, so a placeholder stands
                // in until then.
                pre = "  " + gep + " = getelementptr inbounds " + g->gty + ", " +
                      g->gty + "* @" + g->sym + ", i32 0, " + a3 +
                      (nu ? ", !dx.nonuniform !RQNU" : "") + "\n";
                src = g->elem + "* " + gep;
            }
        }
        return pre + "  " + raw + " = load " + g->elem + ", " + src + ", align 4\n" +
               "  " + instr.result + " = call %dx.types.Handle @" + fn +
               "(i32 160, " + g->elem + " " + raw + ")  ; CreateHandleForLib(Resource)";
    };

    std::map<int, std::optional<std::string>> edits;
    const llm::Function& fn = *q.fn;

    // --- resources ---------------------------------------------------------
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (i.DxOp() == kCreateHandle || i.DxOp() == kBindHandle) {
                const std::string t = handleText(i);
                if (!handleErr.empty()) { r.error = handleErr; return r; }
                edits[i.index] = t;
            }

    // --- no compute thread-index op is legal in a raygen ---------------------
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (IsThreadOp(i.DxOp())) {
                std::string terr;
                const std::string t = ThreadIndexText(i, m, &terr);
                if (!terr.empty()) { r.error = terr; return r; }
                edits[i.index] = t;
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
        const std::set<std::string> types = TypeNames(m.text);
        std::set<std::string> defined, used, handles;
        for (const auto& lbl : q.loop.body) {
            const llm::Block* b = fn.FindBlock(lbl);
            if (!b) continue;
            for (const auto& i : b->instrs) {
                if (!i.result.empty()) defined.insert(i.result);
                for (const auto& u : Operands(i, types)) used.insert(u);
            }
        }
        // A resource handle is not caller state, and neither is anything
        // else the hit shader can work out for itself: a cbuffer read, the ray
        // index, arithmetic on those. See Recomputable above.
        for (const auto& kv : Recomputable(fn, types)) handles.insert(kv.first);

        // But that exemption holds only for a handle the MODULE fully
        // determines. A dynamically indexed one depends on a value computed in
        // the raygen, and recreating it in the any-hit would need that value,
        // which is exactly the caller state the payload cannot carry. So this
        // one IS refused, precisely, rather than being swept up by the generic
        // message below.
        static const std::regex kConstIdx(R"(^i32\s+\d+$)");
        for (const auto& b : fn.blocks) {
            for (const auto& i : b.instrs) {
                if (i.DxOp() != kCreateHandle || i.result.empty()) continue;
                if (!used.count(i.result) || i.args.size() < 4) continue;
                const std::string a3 = Trim(i.args[3]);
                if (std::regex_match(a3, kConstIdx)) continue;
                r.error = "resource handle " + i.result + " is indexed dynamically (" +
                          a3 + ") and used inside the Proceed loop; the index is "
                          "computed in the raygen and the any-hit shader is a "
                          "separate invocation that cannot see it";
                return r;
            }
        }
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
                for (const auto& u : Operands(i, types))
                    if (defined.count(u)) {
                        r.error = "value " + u + " defined in the Proceed loop is used "
                                  "after it; the any-hit shader cannot return it";
                        return r;
                    }
        }

        // A Proceed loop body that WRITES is not transplantable.
        //
        // The check above guards what the body READS. Nothing guarded what it
        // writes, and the two are not the same question. The any-hit shader
        // runs a different number of times than the loop body does, by design:
        // with RAY_FLAG_FORCE_OPAQUE no candidate is ever yielded so it never
        // runs at all, and with FORCE_NON_OPAQUE it runs once per candidate in
        // an implementation-defined order. So a store, an append or an atomic
        // in that body means something different after lowering, silently.
        //
        // Found on RayTracingDebugMainCS, a real Unreal shader that appends a
        // debug record per candidate. It lowered, validated, signed, and then
        // killed the device inside CreateStateObject. Whether the write is
        // also what the driver choked on is NOT established, and this refusal
        // does not rest on it.
        //
        // DXC marks every dx.op declaration with one of three attribute
        // groups and the module says which is which: readnone is pure,
        // readonly loads, a bare nounwind WRITES. Reading that beats a
        // hand-kept list of store opcodes, which would be incomplete the day
        // DXIL grows another one. The numbering is resolved per module rather
        // than assumed, see the attribute group bug in 0.26.0.
        //
        // LIMIT, stated rather than hidden: this reads dx.op calls. A plain
        // `store` reaches only an alloca or groupshared in practice, and
        // groupshared around a query is already refused.
        {
            std::map<std::string, std::string> groupText;
            static const std::regex kAttrLine(
                R"(^attributes\s+#(\d+)\s*=\s*\{(.*)\}\s*$)");
            static const std::regex kDeclLine(
                R"(^declare\s+.*@([\w.$]+)\s*\(.*\)\s*#(\d+)\s*$)");
            std::map<std::string, std::string> byNum;
            std::smatch mm;
            for (const auto& line : m.lines)
                if (std::regex_match(line, mm, kAttrLine))
                    byNum[mm[1].str()] = mm[2].str();
            for (const auto& line : m.lines)
                if (std::regex_match(line, mm, kDeclLine)) {
                    auto it = byNum.find(mm[2].str());
                    groupText[mm[1].str()] = it == byNum.end() ? "" : it->second;
                }
            for (const auto& b : fn.blocks) {
                if (!q.loop.body.count(b.label)) continue;
                for (const auto& i : b.instrs) {
                    if (i.callee.compare(0, 6, "dx.op.") != 0) continue;
                    // The RayQuery ops are mutators too and are all declared
                    // nounwind. They are not transplanted, they are REWRITTEN,
                    // and the analysis already refuses an opcode it does not
                    // know.
                    if (i.callee.compare(0, 15, "dx.op.rayQuery_") == 0 ||
                        i.callee == "dx.op.allocateRayQuery")
                        continue;
                    auto g = groupText.find(i.callee);
                    if (g != groupText.end() &&
                        (g->second.find("readnone") != std::string::npos ||
                         g->second.find("readonly") != std::string::npos))
                        continue;
                    const int op = i.DxOp();
                    r.error = "Proceed loop body has a side effect (" + i.callee +
                              ", opcode " + (op >= 0 ? std::to_string(op) : "?") +
                              "); the any-hit shader it becomes runs a different "
                              "number of times than the loop body does, and in an "
                              "implementation-defined order, so the write would not "
                              "be the same write";
                    return r;
                }
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
            { 9, "i32", 4 }, { 10, "i32", 4 },
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
        // The RayFlags operand, which may be a runtime value. The OR that
        // combines it with the template's flags has to land before the call.
        const auto flags = q.FlagsOperand();
        if (!flags.first.empty()) lines.push_back(flags.first);
        std::ostringstream tr;
        tr << "  call void @dx.op.traceRay." << std::string(kPayload).substr(1)
           << "(i32 157, %dx.types.Handle " << q.AsHandle() << ", " << flags.second
           << ", " << ra[0] << ", i32 0, i32 "
           << (q.needsRecordConstants ? 1 : 0)
           << ", i32 0, " << ra[1] << ", " << ra[2]
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

    // --- RayFlags read in the RAYGEN becomes the constant it must be --------
    //
    // dx.op.rayFlags is legal in a hit or miss shader, not in a raygen, so the
    // loop-body uses go through kCandidateMap and these do not. The value is
    // the same one the lowering hands TraceRay, and the analysis refuses a
    // query whose flags are not a compile-time constant, so there is nothing to
    // look up at runtime.
    //
    // An SSA name needs an instruction to define it, hence `add N, 0`, which
    // the assembler folds away.
    for (const auto& c : q.candidateOps) {
        const llm::Instr& i = *c.second;
        if (i.DxOp() != kRayFlags) continue;
        // A read inside the loop already has an edit: the whole block is marked
        // for deletion and the body is re-emitted into the any-hit, where the
        // intrinsic IS legal. Overwriting that would resurrect it in the raygen
        // as well.
        if (edits.count(i.index)) { q.rayFlagsInLoop = true; continue; }
        if (q.StaticFlags()) {
            edits[i.index] = "  " + i.result + " = add i32 " +
                             std::to_string(q.RayFlags()) + ", 0";
        } else {
            // RayFlags() can only be called after TraceRayInline, so the
            // runtime operand dominates this point.
            edits[i.index] = "  " + i.result + " = or i32 " +
                             llm::OperandName(q.trace->args[3]) + ", " +
                             std::to_string(q.constFlags);
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

    edits[fn.index] = "define void @" + e.raygen + "() #RQNW {";

    std::string out = llm::Render(m, edits);

    // --- types and globals --------------------------------------------------
    {
        std::vector<std::string> decls;
        decls.push_back(std::string(kPayload) + " = type " + kPayloadType);
        decls.push_back(std::string(kAttrs) + " = type { <2 x float> }");
        if (q.needsRecordConstants) {
            // Two words, matching the two root constants the shim puts in every
            // hit group record. Declared here even when the original shader had
            // no cbuffer of its own, which is why CBufRet is conditional too.
            decls.push_back(std::string(kRecordType) + " = type { i32, i32 }");
            if (out.find(std::string(kCbRet) + " = type") == std::string::npos)
                decls.push_back(std::string(kCbRet) + " = type { i32, i32, i32, i32 }");
        }
        decls.push_back("");
        if (q.needsRecordConstants)
            decls.push_back(std::string(kRecordGlobal) + " = external constant " +
                            kRecordType + ", align 4");
        std::set<std::string> seen;
        for (const auto& g : globals) {
            if (!seen.insert(g.sym).second) continue;
            // In the binding form the global holds a HANDLE whatever the
            // resource is; the RECORD bitcasts it back to the resource type.
            // Copied from DXC: `@CB = external constant %dx.types.Handle`.
            decls.push_back("@" + g.sym + " = external constant " +
                            (binding ? std::string(kHandleType) : g.gty) +
                            ", align 4");
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
            // also removes threadIdInGroup, which shares the prefix
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.threadId[^\n]*\n)",
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.groupId[^\n]*\n)",
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.flattenedThreadIdInGroup[^\n]*\n)",
            // The 6.6 handle creation is converted away, so its declare goes
            // too: an unused declare is itself a validation error.
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.createHandleFromBinding[^\n]*\n)",
        };
        for (const char* k : kill) out = std::regex_replace(out, std::regex(k), "\n");

        // DispatchRaysIndex replaces threadId, so a shader that never read
        // SV_DispatchThreadID never calls it, and an unused declare is itself
        // a validation error. See lower.py.
        bool usesThreadId = false;
        for (const auto& b : q.fn->blocks)
            for (const auto& i : b.instrs)
                if (IsThreadOp(i.DxOp())) usesThreadId = true;

        std::vector<std::string> add;
        if (usesThreadId) {
            add.push_back("");
            add.push_back("; Function Attrs: nounwind readnone");
            add.push_back("declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #RQNONE");
        }
        const std::vector<std::string> rest = {
            "", "; Function Attrs: nounwind",
            "declare void @dx.op.traceRay." + std::string(kPayload).substr(1) +
                "(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, "
                "float, float, float, float, float, " + kPayload + "*) #RQNW",
            "", "; Function Attrs: nounwind readonly",
            "declare float @dx.op.rayTCurrent.f32(i32) #RQRO",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.instanceIndex.i32(i32) #RQNONE",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.primitiveIndex.i32(i32) #RQNONE",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.instanceID.i32(i32) #RQNONE",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.hitKind.i32(i32) #RQNONE",
        };
        add.insert(add.end(), rest.begin(), rest.end());
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
        // Only reached from a generated hit shader. A raygen read of RayFlags
        // became a constant, so a shader reading it only there declares
        // nothing, and an unused declare is itself a validation error.
        addDecl(q.rayFlagsInLoop, "declare i32 @dx.op.rayFlags.i32(i32) #RQNONE");
        addDecl(usedOps.count(kCandidateWorldToObject) ||
                usedOps.count(kCommittedWorldToObject),
                "declare float @dx.op.worldToObject.f32(i32, i32, i8) #RQNONE");
        addDecl(usedOps.count(kCandidateObjectRayOrigin) != 0,
                "declare float @dx.op.objectRayOrigin.f32(i32, i8) #RQNONE");
        addDecl(usedOps.count(kCandidateObjectRayDirection) != 0,
                "declare float @dx.op.objectRayDirection.f32(i32, i8) #RQNONE");

        if (q.needsRecordConstants) {
            add.push_back("");
            add.push_back("; Function Attrs: nounwind readonly");
            add.push_back(std::string("declare %dx.types.Handle @") + kRecordHandleFn +
                          "(i32, " + kRecordType + ") #RQRO");
            if (out.find("@dx.op.cbufferLoadLegacy.i32(") == std::string::npos) {
                add.push_back("");
                add.push_back("; Function Attrs: nounwind readonly");
                add.push_back(std::string("declare ") + kCbRet +
                              " @dx.op.cbufferLoadLegacy.i32"
                              "(i32, %dx.types.Handle, i32) #RQRO");
            }
        }
        if (q.NeedsIntersection()) {
            add.push_back("");
            add.push_back("; Function Attrs: nounwind");
            add.push_back("declare i1 @dx.op.reportHit." +
                          std::string(kAttrs).substr(1) + "(i32, float, i32, " +
                          kAttrs + "*) #RQNW");
        }
        // Unconditional now: the generated any-hit may or may not exist, but
        // AnyHitNull always does and it is nothing but an IgnoreHit.
        add.push_back("");
        add.push_back("; Function Attrs: noreturn nounwind");
        add.push_back("declare void @dx.op.ignoreHit(i32) #RQNR");
        if (binding) {
            // One overload serves every resource in the 6.6 form, because the
            // global is a handle whatever the resource is.
            add.push_back("");
            add.push_back("; Function Attrs: nounwind readonly");
            add.push_back(std::string("declare ") + kHandleType +
                          " @dx.op.createHandleForLib.dx.types.Handle(i32, " +
                          kHandleType + ") #RQRO");
        } else {
            std::set<std::string> seenFn;
            for (const auto& g : globals) {
                const std::string f = HandleFn(g.elem);
                if (!seenFn.insert(f).second) continue;
                add.push_back("");
                add.push_back("; Function Attrs: nounwind readonly");
                add.push_back("declare %dx.types.Handle @" + f + "(i32, " + g.elem + ") #RQRO");
            }
        }
        const size_t anchor = out.find("\nattributes #0 =");
        std::string joined;
        for (size_t i = 0; i < add.size(); ++i) {
            if (i) joined += "\n";
            joined += add[i];
        }
        out = out.substr(0, anchor) + "\n" + joined + out.substr(anchor);

    }

    // --- generated shaders ---------------------------------------------------
    {
        std::vector<std::string> fns;
        // ONE loop body, TWO shaders when the query commits both kinds. The
        // substitution of CandidateType is what separates them: folded to
        // CANDIDATE_PROCEDURAL_PRIMITIVE the triangle arm dies and this is an
        // intersection shader, folded to CANDIDATE_NON_OPAQUE_TRIANGLE the
        // procedural arm dies and it is an any-hit. Each keeps the other's
        // dead arm as valid but unreachable IR.
        if (q.NeedsIntersection()) {
            // The Proceed loop body re-rooted as an INTERSECTION shader. Same
            // body as the any-hit case, with one substitution changed: an
            // any-hit folds CandidateType to CANDIDATE_NON_OPAQUE_TRIANGLE (0)
            // and this folds it to CANDIDATE_PROCEDURAL_PRIMITIVE (1), so the
            // triangle branch dies and the procedural branch survives. One
            // body, two substitutions, two shaders.
            //
            // CommitProceduralPrimitiveHit becomes ReportHit. An intersection
            // shader has no accept or reject terminator: reporting IS
            // accepting and returning reports nothing, so every way out of the
            // loop body is a plain ret void.
            std::map<std::string, std::string> subst;
            for (const auto& lbl : q.loop.body) {
                if (lbl == q.loop.latch) continue;
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                for (const auto& i : b->instrs) {
                    if (i.DxOp() == kCandidateType) subst[i.result] = "1";
                    else if (i.DxOp() == kCandidateProcNonOpaque)
                        subst[i.result] = "true";
                    // Triangle barycentrics have NO source in an intersection
                    // shader: there is no attributes parameter to read them
                    // from. HitKind() is not available either. Both appear
                    // only in the triangle arm, which CandidateType has just
                    // folded away, so they are dead. Constants keep the IR
                    // valid without pretending to a value. Substituted in this
                    // pre-pass, so it does not depend on the order blocks
                    // happen to be emitted in.
                    else if (i.DxOp() == kCandidateBary)
                        subst[i.result] = "0.000000e+00";
                    else if (i.DxOp() == kCandidateFrontFace)
                        subst[i.result] = "false";
                }
            }
            std::vector<std::string> order{ q.loop.header };
            {
                std::vector<std::string> rest;
                for (const auto& l : q.loop.body)
                    if (l != q.loop.header && l != q.loop.latch) rest.push_back(l);
                std::sort(rest.begin(), rest.end());
                for (const auto& l : rest) order.push_back(l);
            }
            std::vector<std::string> is;
            is.push_back("define void @" + e.intersection + "() #RQNW {");
            is.push_back(std::string("  %rq.at = alloca ") + kAttrs + ", align 8");

            const std::set<std::string> types = TypeNames(m.text);
            std::set<std::string> usedNames, inBody;
            for (const auto& lbl : q.loop.body) {
                if (lbl == q.loop.latch) continue;
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                for (const auto& i : b->instrs) {
                    for (const auto& u : Operands(i, types)) usedNames.insert(u);
                    if (!i.result.empty()) inBody.insert(i.result);
                }
            }
            // Everything the body reads from outside itself, rebuilt here in
            // source order. A resource handle becomes its library form; a
            // thread id becomes DispatchRaysIndex, which is the SAME ray so the
            // same value; everything else is pure and is emitted as it stood.
            // The isolation check has already refused anything not on the list.
            for (const llm::Instr* pi :
                 NeededChain(fn, Recomputable(fn, types), usedNames, types, inBody)) {
                const llm::Instr& i = *pi;
                if (i.DxOp() == kCreateHandle || i.DxOp() == kBindHandle) {
                    const std::string ht = handleText(i);
                    if (!handleErr.empty()) { r.error = handleErr; return r; }
                    is.push_back(ht);
                } else if (IsThreadOp(i.DxOp())) {
                    std::string terr;
                    const std::string t = ThreadIndexText(i, m, &terr);
                    if (!terr.empty()) { r.error = terr; return r; }
                    is.push_back(t);
                } else {
                    is.push_back(i.line);
                }
            }

            for (const auto& lbl : order) {
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                if (lbl != q.loop.header) {
                    is.push_back("");
                    is.push_back(lbl.substr(1) + ":");
                }
                for (const auto& i : b->instrs) {
                    const int op = i.DxOp();
                    if (op == kCandidateType || op == kCandidateProcNonOpaque ||
                        op == kCandidateBary || op == kCandidateFrontFace) continue;
                    // A TRIANGLE commit, in the arm CandidateType folded away.
                    // Dead, and an intersection shader has nothing to lower it
                    // onto, so it simply goes.
                    if (op == kCommitNonOpaque) continue;
                    if (op == kCommitProcedural) {
                        const std::string tv = llm::OperandName(i.args[2]);
                        is.push_back("  %rq.rh" + std::to_string(i.index) +
                                     " = call i1 @dx.op.reportHit." +
                                     std::string(kAttrs).substr(1) +
                                     "(i32 158, float " + tv + ", i32 0, " + kAttrs +
                                     "* nonnull %rq.at)"
                                     "  ; ReportHit(THit,HitKind,Attributes)");
                        continue;
                    }
                    // Nothing to do for Abort: returning has reported nothing,
                    // and there is no later invocation to gate.
                    if (op == kAbort) continue;
                    if (RecordWord(op) >= 0) {
                        // The record answers, because nothing in the shader
                        // can. The local root signature bound this record's own
                        // constants.
                        for (const auto& l :
                             RecordRead(i.result.substr(1), RecordWord(op), i.result))
                            is.push_back(l);
                        continue;
                    }
                    if (const CandMap* cmap = FindCand(op)) {
                        std::string cargs = "i32 " + std::to_string(cmap->dxop);
                        for (int k = 0; k < cmap->extra; ++k)
                            cargs += ", " + Trim(i.args[2 + k]);
                        is.push_back("  " + i.result + " = call " + cmap->ty + " @" +
                                     cmap->callee + "(" + cargs + ")");
                        continue;
                    }
                    std::string line = i.line;
                    for (const auto& kv : subst)
                        line = std::regex_replace(
                            line, std::regex("\\" + kv.first + "\\b"), kv.second);
                    line = std::regex_replace(
                        line, std::regex("label\\s+\\" + q.loop.latch + "\\b"),
                        "label %rq.done");
                    for (const auto& s2 : Block_successors(line))
                        if (!q.loop.body.count(s2) && s2 != "%rq.done") {
                            r.error = "Proceed loop body branches to " + s2 +
                                      ", outside the loop; no lowering is defined "
                                      "for that";
                            return r;
                        }
                    is.push_back(line);
                }
            }
            is.push_back("");
            is.push_back("rq.done:");
            is.push_back("  ret void");
            is.push_back("}");
            std::string joinedIs;
            for (size_t i = 0; i < is.size(); ++i) {
                if (i) joinedIs += "\n";
                joinedIs += is[i];
            }
            fns.push_back(joinedIs);
        }
        // Not an else: a both-kinds query emits the intersection shader above
        // AND the any-hit below, from the same loop body.
        if (q.hasLoop && (q.NeedsBoth() || !q.NeedsIntersection())) {
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
                    // Meaningless for a triangle candidate, and in the arm
                    // CandidateType has just folded away. Dead, but it still
                    // has to be a value.
                    else if (i.DxOp() == kCandidateProcNonOpaque)
                        subst[i.result] = "false";
            }

            // Abort() has no direct DXR 1.0 equivalent. An any-hit shader
            // can only IgnoreHit (reject and CONTINUE) or
            // AcceptHitAndEndSearch (accept and stop); there is no "reject and
            // stop", which is what a bare Abort needs.
            //
            // So it becomes a payload flag, covering both shapes uniformly.
            // Abort sets it and every later invocation ignores its candidate
            // at once. Commit-then-abort still accepts, because the control
            // flow still falls through; a bare abort still rejects. Either
            // way nothing further is committed, which is what stopping
            // traversal means for the result.
            //
            // Traversal itself carries on, so this is slower than the ideal.
            // AcceptHitAndEndSearch would be exact for commit-then-abort, but
            // only after proving the commit dominates the abort in the same
            // iteration, and correctness comes first.
            const bool aborts = !q.aborts.empty();

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
                         "* nocapture readonly %attr) #RQNW {");
            ah.push_back(std::string("  %rq.ap = getelementptr inbounds ") + kAttrs +
                         ", " + kAttrs + "* %attr, i32 0, i32 0");
            ah.push_back("  %rq.ab = load <2 x float>, <2 x float>* %rq.ap, align 4");

            // Recreate every resource handle the body uses, under the SAME SSA
            // name it had in the raygen, so the transplanted instructions need
            // no rewriting.
            const std::set<std::string> types = TypeNames(m.text);
            std::set<std::string> usedNames, inBody;
            for (const auto& lbl : q.loop.body) {
                if (lbl == q.loop.latch) continue;
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                for (const auto& i : b->instrs) {
                    for (const auto& u : Operands(i, types)) usedNames.insert(u);
                    if (!i.result.empty()) inBody.insert(i.result);
                }
            }
            // Everything the body reads from outside itself, rebuilt here in
            // source order. A resource handle becomes its library form; a
            // thread id becomes DispatchRaysIndex, which is the SAME ray so the
            // same value; everything else is pure and is emitted as it stood.
            // The isolation check has already refused anything not on the list.
            for (const llm::Instr* pi :
                 NeededChain(fn, Recomputable(fn, types), usedNames, types, inBody)) {
                const llm::Instr& i = *pi;
                if (i.DxOp() == kCreateHandle || i.DxOp() == kBindHandle) {
                    const std::string ht = handleText(i);
                    if (!handleErr.empty()) { r.error = handleErr; return r; }
                    ah.push_back(ht);
                } else if (IsThreadOp(i.DxOp())) {
                    std::string terr;
                    const std::string t = ThreadIndexText(i, m, &terr);
                    if (!terr.empty()) { r.error = terr; return r; }
                    ah.push_back(t);
                } else {
                    ah.push_back(i.line);
                }
            }

            if (aborts) {
                ah.push_back(std::string("  %rq.pab = getelementptr inbounds ") +
                             kPayload + ", " + kPayload + "* %p, i32 0, i32 9");
                ah.push_back("  %rq.abv = load i32, i32* %rq.pab, align 4");
                ah.push_back("  %rq.abc = icmp ne i32 %rq.abv, 0");
                ah.push_back("  br i1 %rq.abc, label %rq.reject, label %rq.body");
                ah.push_back("");
                ah.push_back("rq.body:");
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
                    if (op == kCandidateType || op == kCommitNonOpaque ||
                        op == kCandidateProcNonOpaque) continue;
                    // A PROCEDURAL commit, in the arm CandidateType folded
                    // away. Dead, and an any-hit shader cannot report a
                    // procedural hit, so it simply goes.
                    if (op == kCommitProcedural) continue;
                    if (op == kAbort) {
                        ah.push_back("  store i32 1, i32* %rq.pab, align 4");
                        continue;
                    }
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
                    if (RecordWord(op) >= 0) {
                        // The record answers, because nothing in the shader
                        // can. The local root signature bound this record's own
                        // constants.
                        for (const auto& l :
                             RecordRead(i.result.substr(1), RecordWord(op), i.result))
                            ah.push_back(l);
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
                    for (const auto& s2 : Block_successors(line))
                        if (!q.loop.body.count(s2) && s2 != "%rq.accept" &&
                            s2 != "%rq.reject") {
                            r.error = "Proceed loop body branches to " + s2 +
                                      ", outside the loop; no lowering is defined "
                                      "for that";
                            return r;
                        }
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

        // ClosestHit. A both-kinds query needs TWO of these: a triangle hit
        // reports COMMITTED_TRIANGLE_HIT and a procedural one
        // COMMITTED_PROCEDURAL_PRIMITIVE_HIT, and one shader cannot say both,
        // because it does not know which hit group resolved to it.
        auto closestHit = [&](const std::string& name, const char* status) {
            std::vector<std::string> ch;
            ch.push_back("define void @" + name + "(" + kPayload +
                         "* noalias nocapture %p, " + kAttrs +
                         "* nocapture readonly %attr) #RQNW {");
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
            ch.push_back(std::string("  store i32 ") + status +
                         ", i32* %ph, align 4");
            for (const auto& s : kChSource) {
                ch.push_back("  %id" + std::to_string(s.first) + " = " + s.second);
                ch.push_back("  %pi" + std::to_string(s.first) +
                             " = getelementptr inbounds " + kPayload + ", " + kPayload +
                             "* %p, i32 0, i32 " + std::to_string(s.first));
                ch.push_back("  store i32 %id" + std::to_string(s.first) + ", i32* %pi" +
                             std::to_string(s.first) + ", align 4");
            }

            // The record's two numbers, when the shader asked for either.
            // Both are stored whenever either is read: one cbuffer load answers
            // both, so splitting them would cost a branch and save nothing.
            if (q.needsRecordConstants) {
                for (const auto& l : RecordRead("g", 0, "%rq.geo")) ch.push_back(l);
                ch.push_back(std::string("  %rq.gp = getelementptr inbounds ") +
                             kPayload + ", " + kPayload + "* %p, i32 0, i32 5");
                ch.push_back("  store i32 %rq.geo, i32* %rq.gp, align 4");
                ch.push_back(std::string("  %rq.con = extractvalue ") + kCbRet +
                             " %rq.cbrg, 1");
                ch.push_back(std::string("  %rq.cp = getelementptr inbounds ") +
                             kPayload + ", " + kPayload + "* %p, i32 0, i32 10");
                ch.push_back("  store i32 %rq.con, i32* %rq.cp, align 4");
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
        };

        if (q.NeedsBoth()) {
            closestHit(e.closesthit, "1");
            closestHit(e.closesthitproc, "2");
        } else {
            closestHit(e.closesthit, q.NeedsIntersection() ? "2" : "1");
        }

        // Miss
        fns.push_back("define void @" + e.miss + "(" + std::string(kPayload) +
                      "* noalias nocapture %p) #RQNW {\n"
                      "  %ph = getelementptr inbounds " + kPayload + ", " + kPayload +
                      "* %p, i32 0, i32 2\n"
                      "  store i32 0, i32* %ph, align 4\n"
                      "  ret void\n}");

        // The two never-commit stubs. Shapes taken from DXC, see
        // phase5/cases/reference/lib_null_ref.hlsl, which also confirms both
        // are SFI0=0x0 and so carry no Tier 1.1 feature flag.
        //
        // Rejecting every candidate is how a TRIANGLES hit group produces no
        // hit: traversal carries on past that geometry as if the shader had
        // never seen it. #3 is noreturn nounwind, as DXC marks its own.
        fns.push_back("define void @" + e.anyhitnull + "(" + std::string(kPayload) +
                      "* noalias nocapture %p, " + kAttrs +
                      "* nocapture readnone %attr) #RQNR {\n"
                      "  call void @dx.op.ignoreHit(i32 155)  ; IgnoreHit()\n"
                      "  unreachable\n}");

        // And reporting nothing is how a PROCEDURAL hit group produces no hit:
        // an intersection shader that returns has found nothing.
        fns.push_back("define void @" + e.isectnull + "() #RQNW {\n"
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
            if (binding) {
                // `%CB* bitcast (%dx.types.Handle* @CB to %CB*)`, copied from
                // DXC. The global holds a handle; the record still has to name
                // the resource type, so it casts.
                fields[1] = g.gty + "* bitcast (" + kHandleType + "* @" + g.sym +
                            " to " + g.gty + "*)";
            } else {
                fields[1] = std::regex_replace(fields[1], std::regex(R"(\*\s+\S+$)"),
                                               "* @" + g.sym);
            }
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

        // The record cbuffer, added to whatever the application already had.
        //
        // Shape copied from DXC, not invented: a cbuffer record is
        // {id, global, name, space, lowerBound, rangeSize, sizeInBytes, extra}.
        // space 1 keeps it clear of anything the application's own global root
        // signature binds, which the shim must not disturb. See
        // phase5/cases/reference/lib_localroot_ref.hlsl.
        if (q.needsRecordConstants) {
            std::vector<std::string> groups;
            for (const auto& g : SplitTop(md.at(std::stoi(resId))))
                groups.push_back(Trim(g));
            while (groups.size() < 4) groups.push_back("null");
            std::vector<std::string> have;
            if (groups[2] != "null")
                for (const auto& x : SplitTop(md.at(std::stoi(groups[2].substr(1)))))
                    have.push_back(Trim(x));
            const int rec = node("i32 " + std::to_string(have.size()) + ", " +
                                 kRecordType + "* " + kRecordGlobal +
                                 ", !\"rq_record\", i32 1, i32 0, i32 1, i32 8, null");
            std::string grp;
            for (const auto& h : have) grp += h + ", ";
            grp += "!" + std::to_string(rec);
            groups[2] = "!" + std::to_string(node(grp));
            std::string all;
            for (size_t i = 0; i < groups.size(); ++i) {
                if (i) all += ", ";
                all += groups[i];
            }
            // Rewritten in place, so !dx.resources keeps pointing at the same
            // node and nothing else has to learn a new id.
            auto lines = llm::SplitLines(out);
            const int at = llm::FindLine(
                lines, std::regex("^!" + resId + R"( = !\{.*\}\s*$)"));
            if (at >= 0) lines[at] = "!" + resId + " = !{" + all + "}";
            out = llm::JoinLines(lines);
        }

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
        if (q.NeedsBoth()) {
            // An intersection shader is void(), so it annotates like a raygen.
            ann.push_back("void ()* @" + e.intersection);
            ann.push_back("!" + std::to_string(annRaygen));
            ann.push_back("void " + sigH + "* @" + e.anyhit);
            ann.push_back("!" + std::to_string(annHit));
        } else if (q.NeedsIntersection()) {
            // An intersection shader is void(), so it annotates like a raygen.
            ann.push_back("void ()* @" + e.intersection);
            ann.push_back("!" + std::to_string(annRaygen));
        } else if (q.hasLoop) {
            ann.push_back("void " + sigH + "* @" + e.anyhit);
            ann.push_back("!" + std::to_string(annHit));
        }
        ann.push_back("void " + sigH + "* @" + e.closesthit);
        ann.push_back("!" + std::to_string(annHit));
        if (q.NeedsBoth()) {
            ann.push_back("void " + sigH + "* @" + e.closesthitproc);
            ann.push_back("!" + std::to_string(annHit));
        }
        ann.push_back("void " + sigP + "* @" + e.miss);
        ann.push_back("!" + std::to_string(annMiss));
        ann.push_back("void " + sigH + "* @" + e.anyhitnull);
        ann.push_back("!" + std::to_string(annHit));
        // An intersection shader is void(), so it annotates like the raygen.
        ann.push_back("void ()* @" + e.isectnull);
        ann.push_back("!" + std::to_string(annRaygen));
        std::string annJoined;
        for (size_t i = 0; i < ann.size(); ++i) {
            if (i) annJoined += ", ";
            annJoined += ann[i];
        }
        const int annId = node(annJoined);

        const int zero = node("i32 0");
        const int flags = node(
            "i32 0, i64 " +
            std::to_string(ModuleFlags(text0, md) & ~kRt11ShaderFlag));
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

        // Shader kind 8. An intersection shader carries neither a payload
        // size nor an attribute size, exactly as DXC emits it.
        if (q.NeedsBoth()) {
            eps.push_back(entry(e.intersection, "()", 8, false, false));
            eps.push_back(entry(e.anyhit, sigH, 9, true, true));
        } else if (q.NeedsIntersection())
            eps.push_back(entry(e.intersection, "()", 8, false, false));
        else if (q.hasLoop) eps.push_back(entry(e.anyhit, sigH, 9, true, true));
        eps.push_back(entry(e.closesthit, sigH, 10, true, true));
        if (q.NeedsBoth())
            eps.push_back(entry(e.closesthitproc, sigH, 10, true, true));
        eps.push_back(entry(e.miss, sigP, 11, true, false));
        eps.push_back(entry(e.anyhitnull, sigH, 9, true, true));
        eps.push_back(entry(e.isectnull, "()", 8, false, false));
        eps.push_back(entry(e.raygen, "()", 7, false, false));

        std::string epJoined;
        for (size_t i = 0; i < eps.size(); ++i) {
            if (i) epJoined += ", ";
            epJoined += "!" + std::to_string(eps[i]);
        }
        // Allocated LAST, so a shader that does not index dynamically keeps
        // every other node id exactly where it was.
        if (out.find("!RQNU") != std::string::npos)
            out = Replace(out, "!RQNU", "!" + std::to_string(node("i32 1")));

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

    {
        std::string attrErr;
        out = ResolveAttrs(out, &attrErr);
        if (!attrErr.empty()) { r.error = attrErr; return r; }
    }

    r.ok = true;
    r.text = out;
    return r;
}

}  // namespace rq
