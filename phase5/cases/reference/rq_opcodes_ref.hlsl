// What opcode is what, asked of DXC instead of inferred from a gap in a table.
//
// Written because a real Unreal 5.8 run refused 118 shaders on three opcodes
// this project had never seen: 203 (105 times), 214 (12) and 195 (1). Guessing
// from the surrounding numbers would have been easy and is exactly the mistake
// the whitelist exists to prevent: 184 and 185 share one LLVM function, so a
// near miss renders the wrong thing silently.
//
//   dxc -T cs_6_5 -E main phase5\cases\reference\rq_opcodes_ref.hlsl -Fc out.ll
//
// The disassembler prints the opcode name as a trailing comment, so the answer
// is read straight off the output.
//
// Every scalar accessor RayQuery has, so the whole StateScalar family is named
// in one pass rather than one guess at a time.

RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> outBuf : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    RayDesc ray;
    ray.Origin = float3(tid.x / 256.0, tid.y / 256.0, -1);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0.0;
    ray.TMax = 100.0;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

    uint acc = 0;

    // Query-wide, not per-candidate.
    acc += q.RayFlags();
    acc += asuint(q.RayTMin());

    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            acc += q.CandidateInstanceIndex();
            acc += q.CandidateInstanceID();
            acc += q.CandidateGeometryIndex();
            acc += q.CandidatePrimitiveIndex();
            acc += q.CandidateInstanceContributionToHitGroupIndex();
            q.CommitNonOpaqueTriangleHit();
        }
    }

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        acc += q.CommittedInstanceIndex();
        acc += q.CommittedInstanceID();
        acc += q.CommittedGeometryIndex();
        acc += q.CommittedPrimitiveIndex();
        acc += q.CommittedInstanceContributionToHitGroupIndex();
    }

    outBuf[tid.y * 256 + tid.x] = acc;
}
