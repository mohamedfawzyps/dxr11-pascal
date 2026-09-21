// Reference only: what opcodes does DXC emit for the RayQuery accessors
// Unreal uses that this project does not yet support? Compile -T cs_6_5.
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<float4> outBuf : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    RayDesc ray;
    ray.Origin = float3(0, 0, 2); ray.Direction = float3(0, 0, -1);
    ray.TMin = 0; ray.TMax = 10;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    float acc = 0;
    while (q.Proceed()) {
        acc += q.CandidatePrimitiveIndex();
        acc += q.CandidateInstanceIndex();
        acc += q.CandidateInstanceID();
        acc += q.CandidateTriangleRayT();
        acc += q.CandidateObjectRayOrigin().x;
        acc += q.CandidateObjectRayDirection().y;
        acc += q.CandidateWorldToObject3x4()[1][2];
        acc += q.CandidateTriangleFrontFace() ? 1 : 0;
        q.CommitNonOpaqueTriangleHit();
    }
    acc += q.CommittedInstanceID();
    acc += q.CommittedWorldToObject3x4()[0][3];
    acc += q.CommittedTriangleFrontFace() ? 1 : 0;
    outBuf[tid.x] = acc;
}
