// A scene picked per ray at run time, for the shim's own record layout
// (phase5/rewriter/shimtrace.py --caps, 0.60.0): a bounded array at a
// non-uniform element, in a branch whose value a phi picks up after it; an
// unbounded array in a loop, so the key lookup's own loop sits inside the
// application's, back edges on both; with HEAP (lib_6_6) the descriptor heap
// at an index computed in the shader. Not rendered: the rewriter suite checks
// the C++ against the Python and that the result validates; gitest.exe
// --scenearray, --sceneunbounded and --heapdyn render.

cbuffer CB : register(b0) { uint W; uint H; uint Base; uint Pad; };
RaytracingAccelerationStructure Arr[2] : register(t0);
RaytracingAccelerationStructure Unb[] : register(t0, space1);
RWStructuredBuffer<uint4> Out : register(u0);

struct Pay { uint4 v; };

[shader("raygeneration")] void RayGen() {
    uint2 p = DispatchRaysIndex().xy;
    RayDesc r;
    r.Origin = float3(p.x, p.y, 1);
    r.Direction = float3(0, 0, -1);
    r.TMin = 0;
    r.TMax = 10;
    uint acc = 0;
    [branch] if (p.x & 1) {
        Pay a; a.v = 0;
        TraceRay(Arr[NonUniformResourceIndex(p.y & 1)], 0, 0xFF, 0, 1, 0, r, a);
        acc = a.v.x;
    }
    [loop] for (uint i = 0; i < (p.y & 3); ++i) {
        Pay b; b.v = 0;
        TraceRay(Unb[NonUniformResourceIndex(Base + i)], 0, 0xFF, 1, 1, 0, r, b);
        acc += b.v.y;
    }
#if HEAP
    Pay c; c.v = 0;
    RaytracingAccelerationStructure h = ResourceDescriptorHeap[NonUniformResourceIndex(Pad + (p.x & 3))];
    TraceRay(h, 0, 0xFF, 0, 1, 0, r, c);
    acc += c.v.z;
#endif
    Out[p.y * W + p.x] = uint4(acc, 0, 0, 0);
}
