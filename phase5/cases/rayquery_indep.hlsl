// An independently written RayQuery shader.
//
// Everything proven so far descends from the two Phase 2 shaders, which were
// written to demonstrate one lowering. This one is deliberately not: it varies
// the thread group, the control flow shape, the ray setup, the accessor order,
// and it puts the query behind a helper function. Most importantly it does
// what a real alpha test does and READS A RESOURCE INSIDE THE PROCEED LOOP,
// which nothing here has exercised.
//
// It keeps only the harness contract: Result written to u0, so the existing
// diff works. WARP running this same shader is its own ground truth, so the
// alpha rule does not have to match Phase 2's.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<float> alphaMask : register(t1);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

// The query lives behind a call, so the lowering meets it after inlining
// rather than written out at the top level.
Result shade(float2 ndc)
{
    RayDesc ray;
    ray.Origin    = float3(ndc * halfExtent, camZ);
    ray.Direction = float3(0.0, 0.0, -1.0);
    ray.TMin      = tMin;
    ray.TMax      = tMax;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, ~0, ray);

    while (query.Proceed()) {
        // Only non-opaque triangles reach here, same as Phase 2, but the
        // decision comes from a BUFFER rather than arithmetic.
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
            continue;
        float2 bc = query.CandidateTriangleBarycentrics();
        uint slot = ((uint)(bc.x * 8.0)) & 7u;
        if (alphaMask[slot] > 0.5)
            query.CommitNonOpaqueTriangleHit();
    }

    Result res;
    res.hit = (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 1u : 0u;
    if (res.hit != 0u) {
        // Deliberately a different order from Phase 2: barycentrics first.
        float2 cb = query.CommittedTriangleBarycentrics();
        res.bx = cb.x;
        res.by = cb.y;
        res.t  = query.CommittedRayT();
    } else {
        res.bx = 0.0; res.by = 0.0; res.t = 0.0;
    }
    return res;
}

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint x = dtid.x, y = dtid.y;
    if (x >= width) return;
    if (y >= height) return;

    float2 ndc = (float2(x, y) + 0.5) / float2(width, height) * 2.0 - 1.0;
    outBuf[y * width + x] = shade(ndc);
}
