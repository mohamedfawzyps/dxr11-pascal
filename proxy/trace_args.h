// The hit group arguments of an application's TraceRay calls, when they are
// not literals (0.57.0).
//
// TraceRay(AS, flags, mask, R, M, miss, ray, payload) picks a hit group
// record as R + M * GeometryIndex + InstanceContribution, and DXR uses only
// the low 4 bits of R and of M. So whatever a shader computes, a call can use
// at most 16 x 16 pairs, and usually far fewer. The shim needs to know which,
// for GeometryIndex() (proxy/geom_index_so.h): its table labels each record
// with the geometry reaching it under every pair a call can use, and its own
// layout gives every pair a record of its own.
//
// Each argument is followed back through the shader to an expression over
// literals, cbuffer dwords and values it cannot know (a ray index, a payload
// field, a load): `select` and `phi` as either, integer arithmetic exactly,
// `and` with a small mask and `urem` by a small divisor as the values they
// can give. The set of values it can take is evaluated twice: when the
// pipeline is created, with every cbuffer dword unknown, which bounds what the
// shim's own layout must be ready for; and at each dispatch, with the dwords
// read from what the application bound, which is what that dispatch uses.
// Unknown is always every value, so the answer is never too small.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace targs {

// A dword of a cbuffer: register b`reg` in `space`, byte `offset`.
struct Leaf {
    UINT space = 0, reg = 0, offset = 0;
    bool operator==(const Leaf& o) const {
        return space == o.space && reg == o.reg && offset == o.offset;
    }
};

struct Node {
    enum Kind : uint8_t { kConst, kAny, kLeaf, kUnion, kBin, kBool };
    Kind kind = kAny;
    uint32_t value = 0;            // kConst
    Leaf leaf;                     // kLeaf
    std::string op;                // kBin: add, sub, mul, shl, lshr, and, or, xor, udiv, urem
    std::vector<int> kids;         // kUnion: any of them; kBin: the two operands
};

// One TraceRay: its R and M, and the shader kind of the function it is in
// (7 raygen, 10 closest-hit, 11 miss, 0 not known).
struct Site {
    int r = -1, m = -1;
    uint32_t kind = 0;
};

struct Args {
    std::vector<Node> nodes;
    std::vector<Site> sites;
};

// Adds every TraceRay of one disassembled (normalised or not) library.
void Scan(const std::string& text, Args* out);
void Merge(const Args& from, Args* into);

// True when every call's R and M are literals.
bool Literal(const Args& a);

// A cbuffer dword's value at this dispatch, for a call in a shader of `kind`;
// false when it cannot be told.
using LeafValue = std::function<bool(const Leaf& leaf, uint32_t kind, uint32_t* value)>;

// The (R, M) pairs, low 4 bits each, the calls can use, sorted. Without
// `value` every cbuffer dword is unknown.
std::vector<std::pair<UINT, UINT>> Pairs(const Args& a, const LeafValue& value = nullptr);

// DXR_TIER11_TARGS_NOREFINE: every cbuffer dword unknown at the dispatch too.
// The coverage check for the shim's widest layout.
bool NoRefine();

}  // namespace targs
