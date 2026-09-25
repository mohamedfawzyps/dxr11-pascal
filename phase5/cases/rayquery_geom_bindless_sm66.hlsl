// The geom case with the scene taken from the DESCRIPTOR HEAP, Unreal's
// bindless form: every one of Unreal's 64 dumped RayQuery shaders traces
// ResourceDescriptorHeap[i] with i a dword of the cbuffer at b0. Run with
// --geom --contrib --bindless: raytest puts the scene at heap slot 3, the
// index in _pad.x, a conflicting second scene in the slots around it AND in
// the root SRV at t0, so a shim that cannot tell which scene is traced either
// refuses (two live scenes disagree) or picks the wrong one.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; uint sceneIdx; uint _pad1;
};
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

    // NOT forced opaque, so Proceed yields a candidate and the CANDIDATE form
    // is actually exercised. With RAY_FLAG_FORCE_OPAQUE there is no any-hit at
    // all and half of what this tests would never run.
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[sceneIdx];
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

    // The candidate values GATE THE COMMIT rather than escaping the loop.
    //
    // The first shape tried here kept the candidate index in a local and read
    // it after the loop, and the rewriter refused it, correctly: a value
    // defined in the Proceed loop and used after it is caller state the any-hit
    // shader cannot return. Making it decide something inside the loop is both
    // legal and a much stronger test, because a wrong value changes which
    // quadrant is missing from the image.
    //
    // geometry + contribution == 3 is skipped. With --contrib the instance
    // carries 2, so that is geometry 1; without it, geometry 3. The quadrant
    // that disappears MOVES between the two runs, so both accessors have to be
    // right and neither can be a constant.
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            uint g = q.CandidateGeometryIndex();
            uint c = q.CandidateInstanceContributionToHitGroupIndex();
            if (g + c != 3) q.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t  = (float)q.CommittedGeometryIndex();
        r.bx = (float)q.CommittedInstanceContributionToHitGroupIndex();
        r.by = q.CommittedRayT();
        r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
