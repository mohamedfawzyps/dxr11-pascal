// Reference only: a raygen tracing a top-level structure handed to it by a
// LOCAL root descriptor in a register space no application uses, which is how
// the shim points a variant raygen at its own copy of the scene.
RaytracingAccelerationStructure ShimTlas : register(t0, space2147418112);
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<uint> Out : register(u0);
struct Pay { uint v; };
[shader("raygeneration")] void RG() {
    RayDesc r; r.Origin = float3(0, 0, 1); r.Direction = float3(0, 0, -1); r.TMin = 0; r.TMax = 10;
    Pay p; p.v = 0;
    TraceRay(Scene, 0, 0xFF, 0, 0, 0, r, p);
    Pay q; q.v = 0;
    TraceRay(ShimTlas, 0, 0xFF, 1, 2, 0, r, q);
    Out[0] = p.v + q.v;
}
