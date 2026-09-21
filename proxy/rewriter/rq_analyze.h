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

#include "ll_model.h"

namespace rq {

// Verified against DXC 1.10.2605.37 output, see docs/phase5-dxil-recon.md.
enum Opcode {
    kAllocate = 178,
    kTraceInline = 179,
    kProceed = 180,
    kAbort = 181,
    kCommitNonOpaque = 182,
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
};

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
    std::vector<std::pair<const llm::Block*, const llm::Instr*>> candidateOps;
    std::vector<std::pair<const llm::Block*, const llm::Instr*>> committedOps;
    bool hasLoop = false;
    llm::Loop loop;

    int patternNum = 0;
    std::string patternDesc;

    int DynFlags() const;
    int RayFlags() const;
    std::string AsHandle() const;
    // mask, origin x/y/z, tmin, direction x/y/z, tmax, as operand text.
    std::vector<std::string> RayArgs() const;
    bool NeedsAnyHit() const { return hasLoop; }
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
