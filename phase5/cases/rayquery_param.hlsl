// Values computed BEFORE the Proceed loop and read INSIDE it.
//
// This is the ordinary shape of a real alpha test: read the thresholds once,
// compare against them per candidate. Unreal refused 59 shaders on it, with the
// message "Proceed loop body reads values defined outside it".
//
// The refusal was right in principle and wrong here, the same way it was once
// wrong about resource handles. The any-hit shader is a separate invocation, so
// it cannot see the RAYGEN'S LOCALS. But some values are not caller state at
// all, they are just facts the any-hit can work out for itself:
//
//   a cbuffer read     the cbuffer is bound by the GLOBAL root signature and
//                      holds the same bytes for every invocation of the
//                      dispatch
//   the ray index      DispatchRaysIndex() in the any-hit is the SAME ray the
//                      raygen launched, so the same numbers
//   arithmetic on them pure, so recomputing it gives the same answer
//
// So the lowering rebuilds the chain at the top of the generated hit shader
// rather than refusing. Anything NOT on that list is still caller state and is
// still refused: a buffer load the raygen might have written, a phi that
// depends on which path the raygen took, an integer divide that could trap if
// hoisted.
//
// Both routes are exercised, because they are rebuilt differently: `thresh`
// comes from the cbuffer, `parity` from the thread id, which the raygen turns
// into DispatchRaysIndex and the any-hit has to as well.
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

    // Computed OUTSIDE the loop, in the raygen, and read INSIDE it. Neither is
    // a constant the compiler can fold away: both come from something only
    // known at runtime.
    float thresh = 64.0 / (float)width;          // cbuffer -> arithmetic
    uint  parity = (tid.x ^ tid.y) & 1u;         // thread id -> arithmetic

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            float2 b = q.CandidateTriangleBarycentrics();
            // A checkerboard crossed with a threshold, so getting either value
            // wrong changes the image in a way the diff cannot miss.
            if (b.x > thresh && parity == 0u)
                q.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t  = q.CommittedRayT();
        r.bx = thresh;
        r.by = (float)parity;
        r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
