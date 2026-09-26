// Counts of what happened to lowered RayQuery dispatches, and to the scene
// data they depend on, printed as one line every few seconds and once at exit.
//
// Why this exists: the 0.40.0 Escher run could not say how often the open
// world's lowered passes actually drew. Every interesting log line there is
// rate-limited, the refusal to ONE line and the table rebuild to 32, and both
// limits were spent before the open world loaded. A counter cannot be spent.
#pragma once

#include <string>

namespace dstats {

enum Counter {
    kDrawn,               // a DispatchRays recorded for a lowered pipeline
    kIndirect,            // of those, from an indirect compute dispatch
    kIndirectEmpty,       // an indirect dispatch that read back zero groups
    kRefusedCrossLive,    // two live top-level structures disagree on a record
    kRefusedOneTlas,      // one structure puts two pairs on one record
    kRefusedProcedural,   // both geometry kinds on one record, shader commits procedural
    kSkippedTable,        // the table could not be rebuilt
    kHeldBack,            // rqdispatch held it back on purpose
    kTableNew,            // a table built for a layout not seen before
    kTableCached,         // a table taken from the per-pipeline cache
    kTableRecycled,       // a new table written into an idle spare buffer
    kTableFreed,          // a spare buffer released
    kTlasNew,             // instance data read for a new address
    kTlasChanged,         // re-read, and the layout changed
    kTlasSame,            // re-read, and nothing changed
    kSceneResolved,       // the dispatch's scene resolved through its root signature
    kSceneUnresolved,     // not resolved: judged over every live structure
    kRefusedUnread,       // resolved to a structure whose instances are not read yet
    kRefusedUnknownBlas,  // an instance on a bottom-level structure of unknown geometry
    kOwnLayout,           // of those drawn, in the shim's own record layout (0.61.0)
    kCount
};

void Add(Counter c);

// Called on every submission. Prints at most one line every 10 seconds, and
// only when a count has moved. `live` describes the live top-level structures
// and is only evaluated when a line is actually printed.
void Tick(std::string (*live)());

// At process detach, under the loader lock: the counts only, one line, no locks.
void Final();

}  // namespace dstats
