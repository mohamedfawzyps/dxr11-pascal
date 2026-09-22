// Can TraceRay's RayFlags operand be a RUNTIME value, or must it be an
// immediate?
//
// The lowering folded the RayQuery flags into a constant, and refused anything
// else. A real Unreal 5.8 run then refused 118 shaders on exactly that: Epic
// computes ray flags at runtime almost everywhere. If dx.op.traceRay accepts a
// non-constant operand, the fold is unnecessary and the refusal goes away.
//
//   dxc -T lib_6_3 phase5\cases\reference\lib_dynflags_ref.hlsl -Fc out.ll
//
// What to read off the output: whether operand 2 of dx.op.traceRay is an SSA
// value rather than `i32 <literal>`, and whether it validates and signs.
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<float> outBuf : register(u0);
cbuffer CB : register(b0) { uint dynamicFlags; };

struct Payload { float t; };

[shader("raygeneration")]
void RayGen() {
    RayDesc ray;
    ray.Origin = float3(0, 0, -1);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0; ray.TMax = 100;
    Payload p = (Payload)0;
    // Not a literal: the value comes from a cbuffer.
    TraceRay(scene, dynamicFlags, 0xFF, 0, 0, 0, ray, p);
    outBuf[DispatchRaysIndex().x] = p.t;
}

[shader("closesthit")]
void ClosestHit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
    p.t = RayTCurrent();
}

[shader("miss")]
void Miss(inout Payload p) { p.t = -1; }
