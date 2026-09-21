// Reference only: what does the rewriter do with a BINDLESS RayQuery shader,
// the SM 6.6 ResourceDescriptorHeap form every modern NVIDIA and AMD sample
// uses? Compile with -T cs_6_6.
//
// CLAUDE.md claims createHandleFromHeap is refused. The refusal is not in the
// rewriter's source, so this exists to find out what actually happens: a clean
// refusal, a validation failure, or a silently wrong lowering. The answer
// decides whether any bindless engine is a candidate at all.
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    RWStructuredBuffer<Result> outBuf = ResourceDescriptorHeap[0];

    RayDesc ray;
    ray.Origin    = float3(tid.x * 0.01, tid.y * 0.01, 2.0);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = 0.0; ray.TMax = 10.0;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
    q.Proceed();

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t = q.CommittedRayT();
        r.hit = 1;
    }
    outBuf[tid.y * 256 + tid.x] = r;
}
