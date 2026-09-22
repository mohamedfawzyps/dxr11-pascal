// Find and classify RayQuery use in a DXIL module, structurally.
// C++ port of phase5/rewriter/rayquery.py.
//
// Sites are found the way the recon established, not by matching instruction
// text: follow the query handle, which is a plain i32 SSA value; discriminate
// on the opcode IMMEDIATE, never the callee name, because DXC reuses one LLVM
// function for several opcodes; and recognise the rotated Proceed loop through
// real dominator analysis.
//
// Opcodes are a WHITELIST. Anything outside it stops the lowering rather than
// being guessed at. Given that five of the supported opcodes share a single
// LLVM function, a near miss would render the wrong thing silently, which is
// far worse than a refusal.
//
// Errors are returned, not thrown: this compiles into a DLL loaded in an
// application's address space.
#pragma once

#include <set>
#include <string>
#include <vector>
#include <utility>

#include "ll_model.h"

namespace rq {

// Verified against DXC 1.10.2605.37 output, see docs/phase5-dxil-recon.md.
enum Opcode {
    kAllocate = 178,
    kTraceInline = 179,
    kProceed = 180,
    kAbort = 181,
    kCommitNonOpaque = 182,
    kCommitProcedural = 183,
    kCandidateProcNonOpaque = 190,
    kCommittedStatus = 184,
    kCandidateType = 185,
    kCandidateBary = 193,
    kCommittedBary = 194,
    kCommittedRayT = 200,
    kCommittedInstanceIndex = 207,
    kCommittedGeometryIndex = 209,
    kCommittedPrimitiveIndex = 210,
    // Added after surveying Unreal. All read off DXC output, and every DXR 1.0
    // target was compiled and its SFI0 checked first, because GeometryIndex
    // proved an intrinsic can look ordinary and be secretly Tier 1.1.
    kCandidateWorldToObject = 187,
    kCommittedWorldToObject = 189,
    kCandidateFrontFace = 191,
    kCommittedFrontFace = 192,
    kCandidateRayT = 199,
    kCandidateInstanceIndex = 201,
    kCandidateInstanceID = 202,
    kCandidatePrimitiveIndex = 204,
    kCandidateObjectRayOrigin = 205,
    kCandidateObjectRayDirection = 206,
    kCommittedInstanceID = 208,
    // The four the SHADER TABLE answers, because nothing in the shader can.
    // GeometryIndex() in a DXR 1.0 hit shader is itself Tier 1.1, and HLSL
    // exposes no hit-shader intrinsic for the contribution at all. Both were
    // written off for reasons that only hold while the APPLICATION owns the
    // table; the shim builds it, so each record carries these numbers as local
    // root signature constants. Measured at SFI0 = 0x0, see
    // phase5/cases/reference/lib_localroot_ref.hlsl.
    //
    // Numbers read off DXC, not inferred from the gaps around them:
    // phase5/cases/reference/rq_opcodes_ref.hlsl.
    // Query-wide, neither candidate nor committed: the flags the query is
    // tracing with. 59 of Unreal's shaders refused on this once
    // GeometryIndex stopped blocking them first.
    kRayFlags = 195,
    kCandidateGeometryIndex = 203,
    kCandidateInstanceContrib = 214,
    kCommittedInstanceContrib = 215,
};

// Which word of the hit group record an accessor reads, or -1 for the ones
// that do not come from the record at all.
inline int RecordWord(int op) {
    if (op == kCandidateGeometryIndex || op == kCommittedGeometryIndex) return 0;
    if (op == kCandidateInstanceContrib || op == kCommittedInstanceContrib) return 1;
    return -1;
}

// Non-rayQuery opcodes this pass cares about.
enum OtherOpcode {
    kCreateHandle = 57,
    kBarrier = 80,
    kThreadId = 93,
};

bool IsKnownOpcode(int op);
const char* OpcodeName(int op);
// Recognised, but with no lowering on Tier 1.0. Empty if the opcode is fine.
std::string NoLoweringReason(int op);
bool IsCommittedOp(int op);
// Read in the any-hit shader, where they need no payload at all: the
// candidate under test IS what a DXR 1.0 hit-shader intrinsic reports.
bool IsCandidateOp(int op);
std::string FlagNames(int value);

// The compute shader's [numthreads(x,y,z)], read from the entry point's
// properties (tag 4). DispatchRays takes a RAY COUNT where Dispatch takes
// thread GROUPS, and the lowered raygen uses DispatchRaysIndex where the
// original used SV_DispatchThreadID, so the ray grid is groups * numthreads.
// Returns false if the module does not declare one.
bool NumThreads(const llm::Module& m, int out[3]);

struct Query {
    const llm::Function* fn = nullptr;
    std::string handle;
    const llm::Instr* alloc = nullptr;
    int constFlags = 0;
    const llm::Instr* trace = nullptr;
    const llm::Block* traceBlock = nullptr;
    std::vector<std::pair<const llm::Block*, const llm::Instr*>> proceeds;
    std::vector<std::pair<const llm::Block*, const llm::Instr*>> commits;
    std::vector<std::pair<const llm::Block*, const llm::Instr*>> aborts;
    std::vector<std::pair<const llm::Block*, const llm::Instr*>> procCommits;
    std::vector<std::pair<const llm::Block*, const llm::Instr*>> candidateOps;
    std::vector<std::pair<const llm::Block*, const llm::Instr*>> committedOps;
    bool hasLoop = false;
    llm::Loop loop;
    // Set by the lowering: is RayFlags read INSIDE the Proceed loop? Only
    // then does a generated hit shader call dx.op.rayFlags, and only then
    // may it be declared, since an unused declare is a validation error.
    mutable bool rayFlagsInLoop = false;
    // Does any accessor need the answer only the shader TABLE has? Drives the
    // local root signature and the wider hit records, both of which cost
    // something and are therefore only added for a shader that asks.
    bool needsRecordConstants = false;

    int patternNum = 0;
    std::string patternDesc;

    int DynFlags() const;
    // The flags KNOWN AT COMPILE TIME. Classification only: when
    // TraceRayInline is given a runtime value this is just the template's
    // flags, which is the honest answer for deciding whether traversal is
    // provably fixed-function. The traceRay call must use FlagsOperand.
    int RayFlags() const;
    // Are the TraceRayInline flags a compile-time constant?
    bool StaticFlags() const;
    // {setup line, operand text} for traceRay's RayFlags. A runtime value is
    // OR'd with the template's, because RayQuery<FLAGS> means both apply and
    // TraceRay takes only one operand. The setup line is empty when static.
    std::pair<std::string, std::string> FlagsOperand() const;
    std::string AsHandle() const;
    // mask, origin x/y/z, tmin, direction x/y/z, tmax, as operand text.
    std::vector<std::string> RayArgs() const;
    bool NeedsAnyHit() const { return hasLoop && procCommits.empty(); }
    // A procedural commit means the loop body is an INTERSECTION shader.
    bool NeedsIntersection() const { return !procCommits.empty(); }
    // Commits of BOTH kinds, so the loop body becomes TWO shaders. A hit group
    // is either triangles or procedural, never both, so this needs two of
    // them, and two closest-hits as well, because one writes committed status
    // 1 and the other 2. Which record a geometry resolves to is the scene's
    // business, handled by the typed shader table.
    bool NeedsBoth() const { return !commits.empty() && !procCommits.empty(); }
};

struct AnalyzeResult {
    bool ok = false;
    std::string error;    // why it was refused, when !ok
    Query query;
};

// Analyse the single RayQuery in this module, or explain why it cannot be
// lowered.
AnalyzeResult Analyze(const llm::Module& m);

}  // namespace rq
