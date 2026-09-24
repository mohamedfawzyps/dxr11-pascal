// Two RayQuery objects in one entry point, one after the other: the shape of
// 33 of Unreal's Lumen and Niagara shaders, refused as "2 concurrent RayQuery
// objects" until 0.43.0. Each becomes its own TraceRay with its own payload;
// ONE generated any-hit serves both and picks the loop body from a payload
// field the raygen sets before each trace.
//
// The two loop bodies commit OPPOSITE halves of each triangle, so an any-hit
// that ran the wrong body for either trace changes the image. The second
// query also reads a value loaded before it from a UAV, the thread's own
// prefilled output slot (raytest --prefill), which travels in the payload in
// the field after the query id.
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
    uint idx = tid.y * width + tid.x;
    uint pre = outBuf[idx].hit;

    float u = ((tid.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((tid.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    RayQuery<RAY_FLAG_NONE> q1;
    q1.TraceRayInline(scene, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, ray);
    while (q1.Proceed()) {
        if (q1.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            q1.CandidateTriangleBarycentrics().x > 0.5)
            q1.CommitNonOpaqueTriangleHit();
    }
    uint hit1 = q1.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u;
    float t1 = hit1 ? q1.CommittedRayT() : 0.0;

    RayQuery<RAY_FLAG_NONE> q2;
    q2.TraceRayInline(scene, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, ray);
    while (q2.Proceed()) {
        if (q2.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            float2 b = q2.CandidateTriangleBarycentrics();
            if (b.x < 0.5 && ((uint)(b.y * 8.0) + pre) % 2 == 0)
                q2.CommitNonOpaqueTriangleHit();
        }
    }
    uint hit2 = q2.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 0u;

    Result r = (Result)0;
    r.hit = hit1 | (hit2 << 1);
    r.t = t1;
    if (hit2) {
        float2 cb = q2.CommittedTriangleBarycentrics();
        r.bx = cb.x;
        r.by = q2.CommittedRayT();
    }
    outBuf[idx] = r;
}
