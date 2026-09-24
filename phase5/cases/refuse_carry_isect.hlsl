// A REFUSAL case: a value read from a UAV before the loop, used inside a loop
// body that becomes an INTERSECTION shader. 0.42.0 carries such a value in the
// payload for an any-hit; an intersection shader has no payload in DXR 1.0,
// so this has to be refused, and by name.
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

    float limit = outBuf[tid.y * width + tid.x].t;   // a UAV, so carried

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (0.0 - o.z) / d.z;
            if (tz > tMin && tz < limit)
                q.CommitProceduralPrimitiveHit(tz);
        }
    }
    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT) { r.t = q.CommittedRayT(); r.hit = 1; }
    outBuf[tid.y * width + tid.x] = r;
}
