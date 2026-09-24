// A REFUSAL case: a second query traced INSIDE the first one's Proceed loop.
// That loop becomes an any-hit shader, and an any-hit shader cannot call
// TraceRay, so there is nowhere for the inner trace to go. Refused by name.
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
        if (q1.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            RayQuery<RAY_FLAG_FORCE_OPAQUE> q2;
            RayDesc r2 = ray;
            r2.TMax = q1.CandidateTriangleRayT();
            q2.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r2);
            q2.Proceed();
            if (q2.CommittedStatus() == COMMITTED_NOTHING)
                q1.CommitNonOpaqueTriangleHit();
        }
    }
    Result r = (Result)0;
    if (q1.CommittedStatus() == COMMITTED_TRIANGLE_HIT) { r.t = q1.CommittedRayT(); r.hit = 1; }
    outBuf[tid.y * width + tid.x] = r;
}
