// TraceRay arguments computed at run time, for the shim's own record layout
// (phase5/rewriter/shimtrace.py --copies N, 0.57.0). One call in a branch whose
// value a phi picks up after it, one in a loop, so splitting a call's block
// for the switch over the scene structures has phis to rename, a back edge
// among them. Not rendered: the rewriter suite checks the C++ against the
// Python and that the result validates; gitest.exe --dynargs renders.

cbuffer CB : register(b0) { uint W; uint H; uint NInst; uint Pad; };
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<uint4> Out : register(u0);

struct Pay { uint4 v; };

[shader("raygeneration")] void RayGen() {
    uint2 p = DispatchRaysIndex().xy;
    RayDesc r;
    r.Origin = float3(p.x, p.y, 1);
    r.Direction = float3(0, 0, -1);
    r.TMin = 0;
    r.TMax = 10;
    Pay pay;
    pay.v = 0;
    uint v = 7;
    [branch] if (p.x > 3) {
        TraceRay(Scene, 0, 0xFF, Pad & 3, NInst, 0, r, pay);
        v = pay.v.x;
    }
    [loop] for (uint i = 0; i < p.y; ++i) {
        TraceRay(Scene, 0, 0xFF, i & 1, 1, 0, r, pay);
        v += pay.v.y;
    }
    Out[p.y * W + p.x] = v;
}

[shader("miss")] void Miss(inout Pay p) { p.v = 1; }
