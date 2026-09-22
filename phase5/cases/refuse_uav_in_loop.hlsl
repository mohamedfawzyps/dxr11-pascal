// A REFUSAL case, and the boundary of the recomputable exemption.
//
// The generated hit shader may rebuild a cbuffer read, the ray index, and pure
// arithmetic on those, because none of them is caller state. It must NOT
// stretch to a UAV read: the raygen may have written that buffer before the
// loop, so running the load again in a different invocation is not the same as
// carrying the value across.
//
// This exists because the exemption is the kind of thing that widens quietly.
// `param` proves the exemption works; this proves it stops.
//
// Named refuse_* rather than rayquery_*, because build_phase5.bat compiles
// every .hlsl here and the render suite must not pick it up as a case.
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

    float fromUav = outBuf[0].t;          // NOT recomputable

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            float2 b = q.CandidateTriangleBarycentrics();
            if (b.x > fromUav) q.CommitNonOpaqueTriangleHit();
        }
    }
    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) { r.t = q.CommittedRayT(); r.hit = 1; }
    outBuf[tid.y * width + tid.x] = r;
}
