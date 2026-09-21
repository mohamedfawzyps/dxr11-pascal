// Reference only: what does a RayQuery with PROCEDURAL primitives look like?
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<float> outBuf : register(u0);
[numthreads(8,8,1)]
void main(uint3 tid : SV_DispatchThreadID) {
    RayDesc ray; ray.Origin = float3(0,0,2); ray.Direction = float3(0,0,-1);
    ray.TMin = 0; ray.TMax = 10;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            bool nonOpaque = q.CandidateProceduralPrimitiveNonOpaque();
            float tHit = 1.0 + o.z + d.z + (nonOpaque ? 0.5 : 0.0);
            q.CommitProceduralPrimitiveHit(tHit);
        } else {
            q.CommitNonOpaqueTriangleHit();
        }
    }
    outBuf[tid.x] = q.CommittedRayT();
}
