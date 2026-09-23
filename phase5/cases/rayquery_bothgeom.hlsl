// A query committing BOTH kinds, where every commit is GATED on the record's
// instance contribution, and the committed values are written out.
//
// This exists for the baked record constants (proxy/rewriter/rq_bake.h). The
// `geom` case only has triangles, so it exercises baked copies of AnyHit and
// ClosestHit and nothing else. Here the record read lands in all four hit
// shaders that get copied per pair: AnyHit, Isect, ClosestHit and
// ClosestHitProc, and the pipeline has to build a triangle AND a procedural
// hit group per pair.
//
// Run with --mixed --contrib: the triangle instance sits on record 0 with
// contribution 0, the procedural one on record 1 with contribution 1. Each arm
// commits only when the contribution it reads is the one its instance really
// carries, so a copy baked with the wrong pair loses that whole instance
// rather than shifting a value slightly.

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
        uint c = q.CandidateInstanceContributionToHitGroupIndex();
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (0.5 - o.z) / d.z;
            float3 p = o + d * tz;
            if (c == 1 && tz > tMin && tz < tMax && abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q.CommitProceduralPrimitiveHit(tz);
        } else {
            float2 b = q.CandidateTriangleBarycentrics();
            if (c == 0 && b.x + b.y < 0.75)
                q.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    uint st = q.CommittedStatus();
    if (st == COMMITTED_TRIANGLE_HIT) {
        r.t = q.CommittedRayT();
        r.bx = (float)q.CommittedInstanceContributionToHitGroupIndex();
        r.by = (float)q.CommittedGeometryIndex();
        r.hit = 1;
    } else if (st == COMMITTED_PROCEDURAL_PRIMITIVE_HIT) {
        r.t = q.CommittedRayT();
        r.bx = (float)q.CommittedInstanceContributionToHitGroupIndex();
        r.by = (float)q.CommittedGeometryIndex();
        r.hit = 2;
    }
    outBuf[tid.y * width + tid.x] = r;
}
