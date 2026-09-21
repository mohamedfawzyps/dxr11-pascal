// A query that commits BOTH triangle and procedural hits, which is the last
// shader-side gap. Run it on the --mixed scene.
//
// The Proceed loop body has to become TWO shaders from one source: an any-hit
// where CandidateType folds to CANDIDATE_NON_OPAQUE_TRIANGLE and the
// procedural arm dies, and an intersection shader where it folds to
// CANDIDATE_PROCEDURAL_PRIMITIVE and the triangle arm dies. And the committed
// status has to come back as 1 or 2 depending on which kind won, which one
// closest-hit cannot say, so there are two of those as well.
//
// The overlap is made DETERMINISTIC on purpose. The procedural commit reports
// the box's front face at z = 0.5, which is always nearer than the triangle at
// z = 0, so wherever both are hit the procedural one wins and the expected
// answer does not depend on traversal order. That matters because RayQuery
// traversal order is implementation-defined and WARP is the oracle.
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
    float u = ((tid.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((tid.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (0.5 - o.z) / d.z;
            float3 p = o + d * tz;
            if (tz > tMin && tz < tMax && abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q.CommitProceduralPrimitiveHit(tz);
        } else {
            float2 b = q.CandidateTriangleBarycentrics();
            if (b.x + b.y < 0.75)
                q.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    uint st = q.CommittedStatus();
    if (st == COMMITTED_TRIANGLE_HIT) {
        float2 b = q.CommittedTriangleBarycentrics();
        r.t = q.CommittedRayT();
        r.bx = b.x; r.by = b.y;
        r.hit = 1;
    } else if (st == COMMITTED_PROCEDURAL_PRIMITIVE_HIT) {
        r.t = q.CommittedRayT();
        r.hit = 2;
    }
    outBuf[tid.y * width + tid.x] = r;
}
