// GeometryIndex() in an APPLICATION's own DXR 1.0 hit shaders.
// C++ port of phase5/rewriter/geomidx.py, held byte-identical to it; the
// reasoning is written down there.
//
// In short: GeometryIndex() is Tier 1.1 and the GTX 1070's driver rejects a
// library that calls it. Every call becomes a read of one 32-bit constant at
// b0 in register space kGeomIndexSpace; the shim appends that constant to the
// local root signature of each hit group that reads it and writes the
// geometry index into its own copy of the application's shader table.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rq {

// The register space of the constant, b0 in this space.
const unsigned kGeomIndexSpace = 0x7FFF0000u;

// Runs on NORMALISED text. Returns false with *why set when the module cannot
// be lowered (it also uses RayQuery, or already uses the shim's names). A
// module with no GeometryIndex() comes back unchanged with no functions.
// *functions receives the export name of every function that read it.
bool LowerGeometryIndex(const std::string& in, std::string* out,
                        std::vector<std::string>* functions, std::string* why);

// The shim's own record layout (phase5/rewriter/shimtrace.py): every
// TraceRay traces the structure at t0, space kGeomIndexSpace with
// RayContributionToHitGroupIndex = the index of its (R, M) in `pairs` and a
// multiplier of pairs.size(). *calls receives how many there were; a library
// with none comes back unchanged.
bool RetraceToShimScene(const std::string& in,
                        const std::vector<std::pair<unsigned, unsigned>>& pairs,
                        std::string* out, int* calls, std::string* why);

// The name a state object uses for a library function: DXC mangles
// `\01?CH@@YAX...` and exports it as CH.
std::string ExportName(const std::string& fn);

}  // namespace rq
