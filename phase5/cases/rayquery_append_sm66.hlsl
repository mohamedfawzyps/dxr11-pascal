// A Proceed loop that APPENDS a record per candidate: a counter increment and a
// store at the index it returned. The shape Unreal's shared TraceRayInline
// wrapper uses for its ray debug log, which put 47 MegaLights and Lumen shaders
// behind the side-effect refusal until 0.41.0. Compiled at 6.6, as Unreal's are.
//
// Used with --multi --append: two quads side by side, so each ray meets at most
// one candidate and the SET of records does not depend on traversal order.
// raytest writes the records, sorted, beside the output, and the suite compares
// them between WARP's native RayQuery and the 1070's lowered any-hit.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);
RWStructuredBuffer<uint> candLog : register(u1);

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
    q.TraceRayInline(scene, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            uint i = candLog.IncrementCounter();
            candLog[i] = ((tid.y * width + tid.x) << 4) |
                         (q.CandidateInstanceIndex() << 2) | q.CandidatePrimitiveIndex();
            q.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t  = (float)q.CommittedInstanceIndex();
        r.bx = (float)q.CommittedPrimitiveIndex();
        r.by = q.CommittedRayT();
        r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
