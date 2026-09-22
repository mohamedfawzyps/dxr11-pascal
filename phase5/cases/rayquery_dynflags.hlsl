// The same as rayquery_flags.hlsl, with the TraceRayInline flags computed at
// RUNTIME rather than written as a literal.
//
// Unreal does this in 118 shaders, which was the single largest refusal once
// GeometryIndex, RayFlags and loop isolation were closed. The lowering used to
// fold the flags into the TraceRay call, so it could only accept a constant.
// It does not have to: dx.op.traceRay takes RayFlags as an ordinary i32
// operand, measured in phase5/cases/reference/lib_dynflags_ref.hlsl.
//
// The expected answer is identical to the static version, 0x002 | 0x200 = 0x202,
// which is the point: same result, different route to it. If the operand were
// dropped the union would lose its runtime half and RayFlags() would report
// 0x002.
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

    // The flags operand is a RUNTIME value: width is 256, so this is
    // RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES (0x200) but the compiler cannot
    // know that. Folding it to a constant is exactly what this catches.
    uint dynFlags = (width == 256u) ? RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES
                                    : RAY_FLAG_CULL_BACK_FACING_TRIANGLES;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> q;
    q.TraceRayInline(scene, dynFlags, 0xFF, ray);

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
