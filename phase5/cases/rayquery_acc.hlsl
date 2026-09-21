// Exercises the accessors added after surveying Unreal. Every one of these is
// read by Epic's Lumen and RayTracing shaders and none of them was supported
// before.
//
// Candidate accessors are read in the Proceed loop and become plain hit-shader
// intrinsics in the generated any-hit, needing no payload at all. Committed
// accessors are read after the loop and travel back in the payload.
//
// The results are packed into the Result fields so the existing diff checks
// them: WARP and the lowered version must agree on every one.
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

    // Candidate accessors decide the commit and nothing else. A value
    // ACCUMULATED across candidates and read after the loop cannot be lowered:
    // the any-hit shader is a separate invocation and the payload is the only
    // way back. The first version of this shader did exactly that and the
    // rewriter refused it, correctly.
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            float score = q.CandidatePrimitiveIndex()
                        + q.CandidateInstanceIndex() * 2.0
                        + q.CandidateInstanceID() * 4.0
                        + q.CandidateTriangleRayT()
                        + q.CandidateObjectRayOrigin().z
                        + q.CandidateObjectRayDirection().z
                        + q.CandidateWorldToObject3x4()[1][1]
                        + (q.CandidateTriangleFrontFace() ? 8.0 : 0.0);
            if (score > -100.0)
                q.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        // These travel back through the payload.
        r.t  = (float)q.CommittedInstanceID()
             + (q.CommittedTriangleFrontFace() ? 16.0 : 0.0);
        r.bx = q.CommittedWorldToObject3x4()[2][2];
        r.by = q.CommittedRayT();
        r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
