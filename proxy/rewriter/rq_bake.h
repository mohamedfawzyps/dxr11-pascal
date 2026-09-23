#pragma once

// Bake the shader-record constants into the hit shaders as immediates.
//
// The C++ port of phase5/rewriter/bake.py, and it must produce IDENTICAL text.
// See that file for the reasoning; in one line: a hit shader reading a cbuffer
// bound by a LOCAL root signature makes the Pascal driver crash inside
// CreateStateObject, so the record carries nothing but an identifier and each
// (geometryIndex, instanceContribution) pair gets its own copy of the hit
// shaders instead. phase5/cases/driver-crash/README.md has the measurements.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rq {

using RecordPair = std::pair<uint32_t, uint32_t>;   // geometryIndex, contribution

// Where the copies of `name` are exported: name + "_" + k.
std::string BakedName(const std::string& name, size_t k);

// The hit shaders copied per pair, in emission order. Whichever of them the
// lowered module defines are copied, so a hit group's shaders always share a
// suffix.
extern const char* const kBakedHitFunctions[4];

// 64 copies were measured clean on the 1070; this is a ceiling for a
// pathological scene, not a measured limit.
const size_t kMaxBakedPairs = 1024;

struct BakeResult {
    bool ok = false;
    std::string error;
    std::string text;
};

BakeResult Bake(const std::string& lowered, const std::vector<RecordPair>& pairs);

}  // namespace rq
