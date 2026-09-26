// The bindless geom case with the scene picked PER RAY: odd columns trace
// ResourceDescriptorHeap[sceneIdx + 1], even ones [sceneIdx]. Run with
// --geom --contrib --bindless: raytest puts the scene at heap slot 3 and a
// second scene at slot 4 (and 1 and 2), its one instance's contribution one
// higher, so record c + 1 is geometry 1 in the first and geometry 0 in the
// second. The dispatch traces both, and the application's records cannot
// carry the pair for both: until 0.61.0 the shim refused it ("two live
// top-level structures put different (contribution, geometry) pairs on hit
// group record"), and drew nothing. The shim's own record layout gives each
// scene's instances records of their own, the scene picked by the key.
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

    RaytracingAccelerationStructure scene =
        ResourceDescriptorHeap[NonUniformResourceIndex(sceneIdx + (tid.x & 1))];
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

    // geometry + contribution == 3 is skipped, so the quadrant that
    // disappears differs between the two scenes: a ray traced against the
    // wrong one, or reporting the other scene's pair, loses a different one.
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
