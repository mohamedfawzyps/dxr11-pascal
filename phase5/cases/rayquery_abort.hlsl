// Abort(), used the way real code uses it: commit the first candidate that
// passes and stop looking.
//
// WHAT THIS CAN AND CANNOT CHECK. Abort is inherently order-dependent: its
// effect is to stop at whichever candidate traversal happens to reach first,
// and that order is implementation-defined, so WARP and NVIDIA may legitimately
// visit in different orders. Anything order-dependent, t or barycentrics or
// which instance was hit, therefore CANNOT be compared between them.
//
// So this outputs only WHETHER there was a hit, which is order-independent:
// with commit-then-abort, a hit occurs exactly when some candidate passes the
// alpha test, whatever order they are seen in.
//
// That still has teeth. If the abort flag were initialised wrong, or the
// any-hit prologue checked it wrongly, every candidate after the first would be
// ignored, or all of them would, and the hit count would collapse.
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
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            float2 b = q.CandidateTriangleBarycentrics();
            if (frac(b.x * 4.0) < 0.5) {
                q.CommitNonOpaqueTriangleHit();
                q.Abort();          // enough, stop looking
            }
        }
    }

    Result r = (Result)0;
    // Order-independent by construction: see the note above.
    r.hit = (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 1u : 0u;
    outBuf[tid.y * width + tid.x] = r;
}
