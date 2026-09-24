// A REFUSAL case: two queries in one entry point, one of which commits
// PROCEDURAL hits and so becomes an intersection shader. Several queries share
// one generated hit group and pick their loop body from the payload; an
// intersection shader has no payload in DXR 1.0, so it cannot tell which query
// it serves. Refused by name until that has another route.
//
// Named refuse_* so the render suite does not pick it up as a case.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    RayDesc ray;
    ray.Origin = float3(0, 0, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    RayQuery<RAY_FLAG_NONE> q1;
    q1.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q1.Proceed()) {
        if (q1.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            q1.CommitNonOpaqueTriangleHit();
    }
    uint h1 = q1.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u;

    RayQuery<RAY_FLAG_NONE> q2;
    q2.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q2.Proceed()) {
        if (q2.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)
            q2.CommitProceduralPrimitiveHit(1.0);
    }
    uint h2 = q2.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT ? 1u : 0u;
    Result r = (Result)0;
    r.hit = h1 | (h2 << 1);
    outBuf[tid.y * width + tid.x] = r;
}
