// The same shape as rayquery_indep.hlsl, compiled as Shader Model 6.6.
//
// 6.6 is not a variation on 6.5 for this rewriter: it reaches a resource
// through createHandleFromBinding (217) followed by annotateHandle (216),
// where 6.5 uses createHandle (57). That one difference accounted for 124 of
// the 157 refusals a real Unreal run produced, because Unreal compiles its
// RayQuery shaders at 6.6 and this rewriter was built against 6.5.
//
// It reads a resource INSIDE the Proceed loop on purpose. That is the case
// where the conversion has to work twice: once in the raygen, and again in the
// generated any-hit shader, which recreates the handle for itself.
//
// Nothing here is 6.6-specific in the HLSL. The shader model is the test, so
// build_phase5.bat compiles every *sm66.hlsl at cs_6_6 after the 6.5 pass.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<float> alphaMask : register(t1);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

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
