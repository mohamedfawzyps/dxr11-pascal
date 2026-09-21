// Lower an analysed RayQuery compute shader into a DXR library.
// C++ port of phase5/rewriter/lower.py.
//
// Every edit is driven by the analysis, never by matching instruction text.
// The module is edited IN PLACE: the application's own instructions never move
// between modules, so its bindings and handles cannot be got wrong in transit,
// and lines nobody touches pass through byte for byte.
//
// Correctness of the port is established by requiring byte-identical output to
// the Python original on every case in the regression, so the code below
// deliberately mirrors its structure and string formatting exactly, even where
// C++ would naturally do something else.
#pragma once

#include <string>

#include "ll_model.h"
#include "rq_analyze.h"

namespace rq {

struct LowerResult {
    bool ok = false;
    std::string error;
    std::string text;      // the lowered .ll, when ok
};

struct Exports {
    std::string raygen = "RayGen";
    std::string anyhit = "AnyHit";
    std::string closesthit = "ClosestHit";
    std::string miss = "Miss";
    std::string intersection = "Isect";
    // Hit groups that must never commit. A scene can route triangle and
    // procedural geometry to DIFFERENT records, and the shim has a real hit
    // group for only one of those kinds; the other index gets a record of the
    // right TYPE that finds nothing. Emitted always, because which one a scene
    // needs is not knowable when the shader is lowered: the acceleration
    // structures do not exist yet. Two tiny functions is a cheap price for not
    // having to re-lower later.
    std::string anyhitnull = "AnyHitNull";
    std::string isectnull = "IsectNull";
};

LowerResult Lower(const llm::Module& m, const Query& q, const Exports& e = Exports());

}  // namespace rq
