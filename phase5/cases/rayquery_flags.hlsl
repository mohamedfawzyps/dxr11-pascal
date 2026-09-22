// RayFlags(), read BOTH inside the Proceed loop and outside it.
//
// 59 of Unreal's shaders refused on this opcode (195) once GeometryIndex
// stopped blocking them first, which made it the largest single remaining
// bucket and the cheapest to close.
//
// The two places take different routes, which is the whole reason this shader
// reads it twice:
//
//   inside the loop    the loop body becomes an any-hit shader, where
//                      dx.op.rayFlags (144) is legal and returns the flags
//                      TraceRay was given
//   outside the loop   the raygen, where dx.op.rayFlags is NOT legal. The
//                      value is known at lowering time, because it is exactly
//                      what the lowering passes to TraceRay, so it becomes a
//                      constant
//
// A lowering that used the constant in both places would still pass a test
// that only read it in one.
//
// The flags here are deliberately a UNION of two sources: the template
// argument and the TraceRayInline argument. RayFlags() returns both OR'd
// together, so 0x002 | 0x200 = 0x202, and a lowering that dropped either half
// reports the wrong number.
//
// Neither flag changes what this scene renders, which is the point: the first
// version used CULL_BACK_FACING_TRIANGLES and culled the only triangle, so the
// oracle reported 0 hits on both sides and the test could not have failed.
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

    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ray);

    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            // Read in the loop, and made to MATTER: if the flags come back
            // wrong here, nothing commits and the image is empty.
            if ((q.RayFlags() & RAY_FLAG_FORCE_NON_OPAQUE) != 0)
                q.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t  = q.CommittedRayT();
        // Read in the raygen, where it must become the folded constant.
        r.bx = (float)q.RayFlags();
        r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
