// A phi whose predecessor is the ENTRY BLOCK.
//
// An `if` with no `else`, straight out of the entry block, makes the merge
// block's phi name the entry as an incoming predecessor:
//
//     %11 = phi float [ 5.000000e-01, %9 ], [ 0.000000e+00, %0 ]
//
// DXC gives the entry block no label, because nothing can branch to it, so
// normalisation had nothing to rename and %0 became a reference to a block
// that was never defined. The assembler said "use of undefined value '%bb0'".
//
// This survived the whole of Phase 5 because EVERY case here opened with
// `if (tid.x >= width) return;`. That guard puts a block between the entry
// and everything else, so the entry is never a phi predecessor. One shared
// habit across every test, hiding one bug.
//
// [branch] stops DXC flattening the if into a select, which would produce no
// phi at all.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    // An if with no else, straight out of the entry block: the merge block's
    // phi has the ENTRY BLOCK as one of its incoming predecessors.
    float bias = 0.0;
    [branch] if (width > 100u) bias = 0.5;

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
            if (b.x > 0.25) q.CommitNonOpaqueTriangleHit();
        }
    }
    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t = q.CommittedRayT() + bias; r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
