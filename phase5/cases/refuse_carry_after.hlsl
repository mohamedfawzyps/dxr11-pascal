// A REFUSAL case: the loop body reads a value computed AFTER TraceRayInline,
// between the trace and the loop. 0.42.0 carries values read before the loop
// in the payload, which the raygen fills just before TraceRay; this one does
// not exist yet at that point, so it has to be refused, and by name.
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

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    float late = outBuf[tid.y * width + tid.x].t;    // read after the trace
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            float2 b = q.CandidateTriangleBarycentrics();
            if (b.x > late) q.CommitNonOpaqueTriangleHit();
        }
    }
    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) { r.t = q.CommittedRayT(); r.hit = 1; }
    outBuf[tid.y * width + tid.x] = r;
}
