#include "rq_analyze.h"

#include <algorithm>
#include <map>
#include <regex>

namespace rq {
namespace {

const std::map<int, const char*>& KnownTable() {
    static const std::map<int, const char*> t = {
        { kAllocate, "AllocateRayQuery" },
        { kTraceInline, "TraceRayInline" },
        { kProceed, "Proceed" },
        { kCommitNonOpaque, "CommitNonOpaqueTriangleHit" },
        { kCommittedStatus, "CommittedStatus" },
        { kCandidateType, "CandidateType" },
        { kCandidateBary, "CandidateTriangleBarycentrics" },
        { kCommittedBary, "CommittedTriangleBarycentrics" },
        { kCommittedRayT, "CommittedRayT" },
        { kCommittedInstanceIndex, "CommittedInstanceIndex" },
        { kCommittedGeometryIndex, "CommittedGeometryIndex" },
        { kCommittedPrimitiveIndex, "CommittedPrimitiveIndex" },
    };
    return t;
}

int Imm(const std::string& arg) {
    static const std::regex re(R"(^i32\s+(-?\d+)$)");
    std::smatch m;
    std::string t = arg;
    t.erase(0, t.find_first_not_of(" \t"));
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    if (std::regex_match(t, m, re)) return std::stoi(m[1].str());
    return 0;
}

}  // namespace

bool IsKnownOpcode(int op) { return KnownTable().count(op) != 0; }

const char* OpcodeName(int op) {
    auto it = KnownTable().find(op);
    return it == KnownTable().end() ? "?" : it->second;
}

std::string NoLoweringReason(int op) {
    // CommittedGeometryIndex is recognised so the refusal can explain itself,
    // but it has NO lowering on this hardware. Its DXR 1.0 equivalent,
    // GeometryIndex() in a hit shader, is ITSELF a Tier 1.1 feature: measured,
    // such a library sets shader flag 0x2000000 and CreateStateObject on the
    // GTX 1070 fails with E_INVALIDARG. Encoding the index in the shader table
    // would work but means the shim rebuilding the application's SBT.
    if (op == kCommittedGeometryIndex)
        return "CommittedGeometryIndex has no lowering on Tier 1.0. Its DXR 1.0 "
               "equivalent, GeometryIndex() in a hit shader, is itself a Tier 1.1 "
               "feature and CreateStateObject rejects it on this hardware. "
               "Encoding the index in the shader table would work but is not "
               "implemented.";
    return "";
}

bool IsCommittedOp(int op) {
    return op == kCommittedStatus || op == kCommittedBary || op == kCommittedRayT ||
           op == kCommittedInstanceIndex || op == kCommittedGeometryIndex ||
           op == kCommittedPrimitiveIndex;
}

std::string FlagNames(int value) {
    static const std::pair<int, const char*> kFlags[] = {
        { 0x001, "FORCE_OPAQUE" }, { 0x002, "FORCE_NON_OPAQUE" },
        { 0x004, "ACCEPT_FIRST_HIT_AND_END_SEARCH" }, { 0x008, "SKIP_CLOSEST_HIT_SHADER" },
        { 0x010, "CULL_BACK_FACING_TRIANGLES" }, { 0x020, "CULL_FRONT_FACING_TRIANGLES" },
        { 0x040, "CULL_OPAQUE" }, { 0x080, "CULL_NON_OPAQUE" },
        { 0x100, "SKIP_TRIANGLES" }, { 0x200, "SKIP_PROCEDURAL_PRIMITIVES" },
    };
    std::string out;
    for (const auto& f : kFlags) {
        if (!(value & f.first)) continue;
        if (!out.empty()) out += "|";
        out += f.second;
    }
    return out.empty() ? "NONE" : out;
}

int Query::DynFlags() const { return trace ? Imm(trace->args[3]) : 0; }
int Query::RayFlags() const { return constFlags | DynFlags(); }

std::string Query::AsHandle() const {
    return trace ? llm::OperandName(trace->args[2]) : "";
}

std::vector<std::string> Query::RayArgs() const {
    std::vector<std::string> out;
    if (!trace) return out;
    for (size_t i = 4; i < trace->args.size() && i <= 12; ++i)
        out.push_back(trace->args[i]);
    return out;   // mask, ox, oy, oz, tmin, dx, dy, dz, tmax
}

namespace {

std::string RejectModule(const llm::Module& m) {
    std::smatch mm;
    static const std::regex kSm(R"(!dx\.shaderModel\s*=\s*!\{!(\d+)\})");
    if (std::regex_search(m.text, mm, kSm)) {
        const std::regex node("!" + mm[1].str() +
                              R"RX( = !\{!"(\w+)", i32 (\d+), i32 (\d+)\}\s*)RX");
        const int at = llm::FindLine(llm::SplitLines(m.text), node);
        if (at >= 0) {
            std::smatch nm;
            const std::string line = llm::SplitLines(m.text)[at];
            std::regex_match(line, nm, node);
            if (nm[1].str() != "cs")
                return "RayQuery in a \"" + nm[1].str() + "\" shader. DispatchRays only "
                       "launches raygen, so there is no promotion path from any other stage.";
        }
    }
    if (m.text.find("addrspace(3)") != std::string::npos)
        return "shader uses groupshared memory; a raygen shader has no thread group";
    return "";
}

std::string Collect(const llm::Function& fn, Query& q) {
    for (const auto& b : fn.blocks) {
        for (const auto& i : b.instrs) {
            const int op = i.DxOp();
            if (op < 0) continue;
            const auto uses = i.Uses();
            const bool usesHandle =
                std::find(uses.begin(), uses.end(), q.handle) != uses.end();
            if (!usesHandle) {
                if (IsKnownOpcode(op) && op != kAllocate)
                    return std::string(OpcodeName(op)) + " at \"" +
                           i.body.substr(0, 60) + "\" does not use the query handle";
                continue;
            }
            if (!IsKnownOpcode(op)) {
                return "unrecognised rayQuery opcode " + std::to_string(op) + " at \"" +
                       i.body.substr(0, 60) + "\". This project has only verified the "
                       "opcodes in the whitelist. Refusing rather than guessing.";
            }
            const std::string no = NoLoweringReason(op);
            if (!no.empty()) return no;

            if (op == kTraceInline) {
                if (q.trace) return "more than one TraceRayInline on one query";
                q.trace = &i; q.traceBlock = &b;
            } else if (op == kProceed) {
                q.proceeds.emplace_back(&b, &i);
            } else if (op == kCommitNonOpaque) {
                q.commits.emplace_back(&b, &i);
            } else if (op == kCandidateType || op == kCandidateBary) {
                q.candidateOps.emplace_back(&b, &i);
            } else if (IsCommittedOp(op)) {
                q.committedOps.emplace_back(&b, &i);
            }
        }
    }
    if (!q.trace) return "query is allocated but never traced";
    return "";
}

std::string FindLoop(const llm::Function& fn, Query& q) {
    std::set<std::string> proceedBlocks;
    for (const auto& p : q.proceeds) proceedBlocks.insert(p.first->label);

    std::vector<llm::Loop> found;
    for (const auto& lp : fn.NaturalLoops())
        if (proceedBlocks.count(lp.latch)) found.push_back(lp);

    if (found.size() > 1) return "more than one Proceed loop on one query";
    if (found.size() == 1) { q.hasLoop = true; q.loop = found[0]; return ""; }
    if (q.proceeds.size() > 1)
        return std::to_string(q.proceeds.size()) +
               " Proceed calls but no loop; unrecognised shape";
    return "";
}

std::string RejectFunction(const llm::Function& fn, const Query& q) {
    for (const auto& b : fn.blocks) {
        for (const auto& i : b.instrs) {
            const int op = i.DxOp();
            if (op == kBarrier)
                return "shader uses a group barrier; a raygen shader has no thread group";
            std::string c = i.callee;
            std::transform(c.begin(), c.end(), c.begin(), ::tolower);
            if (!c.empty() && c.find(".wave") != std::string::npos)
                return "shader uses wave intrinsics around the query; promotion to "
                       "raygen changes lane occupancy";
        }
    }
    if (q.hasLoop) {
        for (const auto& label : q.loop.body) {
            const llm::Block* blk = fn.FindBlock(label);
            if (!blk) continue;
            for (const auto& i : blk->instrs)
                if (IsCommittedOp(i.DxOp()))
                    return "committed state is read inside the Proceed loop; the "
                           "any-hit shader is a separate invocation and cannot see it";
        }
    }
    return "";
}

}  // namespace

AnalyzeResult Analyze(const llm::Module& m) {
    AnalyzeResult r;
    const std::string modErr = RejectModule(m);
    if (!modErr.empty()) { r.error = modErr; return r; }

    std::vector<const llm::Function*> cands;
    for (const auto& f : m.functions)
        if (!f.FindOp(kAllocate).empty()) cands.push_back(&f);
    if (cands.empty()) { r.error = "no dx.op.allocateRayQuery in this module"; return r; }
    if (cands.size() > 1) {
        r.error = "RayQuery used in " + std::to_string(cands.size()) +
                  " functions; only one entry point is supported";
        return r;
    }
    const llm::Function& fn = *cands[0];

    const auto allocs = fn.FindOp(kAllocate);
    if (allocs.size() != 1) {
        // TraceRay has one payload and one in-flight trace, so concurrent
        // queries have no lowering. Named in the brief's table.
        r.error = std::to_string(allocs.size()) + " concurrent RayQuery objects in " +
                  fn.name + "; TraceRay has one payload and one in-flight trace";
        return r;
    }

    Query q;
    q.fn = &fn;
    q.alloc = allocs[0];
    q.handle = allocs[0]->result;
    q.constFlags = Imm(allocs[0]->args[1]);

    std::string err = Collect(fn, q);
    if (!err.empty()) { r.error = err; return r; }
    err = FindLoop(fn, q);
    if (!err.empty()) { r.error = err; return r; }
    err = RejectFunction(fn, q);
    if (!err.empty()) { r.error = err; return r; }

    // Classify here rather than on demand, so a shape with no defined lowering
    // is refused by analysis itself.
    if (q.NeedsAnyHit()) {
        q.patternNum = 3;
        q.patternDesc = "alpha-tested closest hit (generated any-hit shader)";
    } else if (q.RayFlags() & 0x004) {
        q.patternNum = 2;
        q.patternDesc = "shadow / visibility (miss shader only)";
    } else if (q.RayFlags() & 0x001) {
        q.patternNum = 1;
        q.patternDesc = "opaque closest hit (no any-hit shader)";
    } else {
        r.error = "query has no Proceed loop but is not FORCE_OPAQUE (flags " +
                  FlagNames(q.RayFlags()) + "); no lowering is defined for this";
        return r;
    }

    r.ok = true;
    r.query = q;
    return r;
}

}  // namespace rq
