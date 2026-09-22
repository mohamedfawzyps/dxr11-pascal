// A Proceed loop body containing real CONTROL FLOW, so DXC emits a phi inside
// the loop.
//
// Unreal's loop bodies are full of these, and the isolation check refused 118
// shaders because of them, reporting that the body "reads values defined
// outside it (%bb1, %bb2, ...)". Those are not values at all: a phi writes its
// predecessors as `[ %val, %bb12 ]`, with no `label` keyword to mark them, so
// the operand scan counted the predecessor BLOCKS as value reads.
//
// The bug was invisible for a version, because these same shaders were being
// refused earlier for computing their ray flags at runtime. Closing that
// refusal is what exposed this one.
//
// [branch] is there to stop DXC flattening the if into a select, which would
// produce no phi and test nothing.
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

    float thresh = 64.0 / (float)width;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);

    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            float2 b = q.CandidateTriangleBarycentrics();
            float w;
            [branch] if (b.x > b.y) {
                w = b.x - thresh;
            } else {
                w = b.y * 2.0;
            }
            if (w > thresh) q.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t  = q.CommittedRayT();
        r.bx = q.CommittedTriangleBarycentrics().x;
        r.by = q.CommittedTriangleBarycentrics().y;
        r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
