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
};

LowerResult Lower(const llm::Module& m, const Query& q, const Exports& e = Exports());

}  // namespace rq
