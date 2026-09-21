// Reference only: which DXR 1.0 intrinsics are available in an any-hit or
// closest-hit shader, and which of them secretly require Tier 1.1?
//
// GeometryIndex() was the trap: it looks like a normal hit-shader intrinsic
// and is actually Tier 1.1, so a library using it is refused by the driver on
// Pascal. Before claiming any other accessor is "easy to add", check the same
// way: compile, then look at the shader feature flags.
struct P { float a; float3 b; uint c; };
[shader("anyhit")]
void AH(inout P p, BuiltInTriangleIntersectionAttributes attr) {
    p.c = PrimitiveIndex();          // Candidate/CommittedPrimitiveIndex
    p.c += InstanceIndex();          // Candidate/CommittedInstanceIndex
    p.c += InstanceID();             // Candidate/CommittedInstanceID
    p.c += HitKind();                // *TriangleFrontFace
    p.a = RayTCurrent();             // CandidateTriangleRayT / CommittedRayT
    p.b = ObjectRayOrigin();         // CandidateObjectRayOrigin
    p.b += ObjectRayDirection();     // CandidateObjectRayDirection
    p.b += mul(float4(p.b, 1), WorldToObject4x3());   // *WorldToObject
}
