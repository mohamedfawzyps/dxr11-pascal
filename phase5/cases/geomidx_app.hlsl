// The application library tier11/gitest.cpp builds, kept here so the rewriter
// suite can lower it without the harness. Compile with -D R_VAL=0 -D M_VAL=1
// -D ANYHIT=1, at lib_6_5 and lib_6_6.

cbuffer CB : register(b0) { uint W; uint H; uint NInst; uint Pad; };
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<uint4> Out : register(u0);

cbuffer Rec : register(b0, space1) { uint RecTag; };
cbuffer Rec3 : register(b1, space1) { uint RecA; uint RecB; uint RecC; };

struct Pay { uint4 v; };

[shader("raygeneration")] void RayGen() {
    uint2 p = DispatchRaysIndex().xy;
    RayDesc r;
    r.Origin = float3((p.x + 0.5) / W * NInst, (p.y + 0.5) / H, 1.0);
    r.Direction = float3(0, 0, -1);
    r.TMin = 0.0; r.TMax = 10.0;
    Pay pay; pay.v = uint4(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
#if ANYHIT
    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, R_VAL, M_VAL, 0, r, pay);
#else
    TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, R_VAL, M_VAL, 0, r, pay);
#endif
    Out[p.y * W + p.x] = pay.v;
}

[shader("miss")] void Miss(inout Pay p) { p.v = uint4(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF); }

[shader("closesthit")]
void ClosestHit(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    p.v = uint4(GeometryIndex(), InstanceIndex(), PrimitiveIndex(), RecTag);
}
[shader("closesthit")]
void ClosestHit3(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    p.v = uint4(GeometryIndex() + 100, InstanceIndex(), PrimitiveIndex(), RecA + RecB * 7 + RecC * 31);
}
// Never reads GeometryIndex: the shim must leave it, and its records, alone.
[shader("closesthit")]
void ClosestHitOther(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    p.v = uint4(777, InstanceIndex(), PrimitiveIndex(), RecTag);
}
[shader("anyhit")]
void AnyHit(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    if (GeometryIndex() == 1) IgnoreHit();
}
