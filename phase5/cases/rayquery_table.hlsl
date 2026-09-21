// RayQuery bound through a descriptor table, with the UAV in a resource ARRAY.
//
// A descriptor table on its own may not change the DXIL at all, since a
// non-library shader reaches resources through createHandle(rangeId, index)
// whatever the root signature says. What a table actually enables, and what
// does change the DXIL, is an ARRAY of resources with a real index operand.
// This writes through outBufs[2], so createHandle carries index 2 rather
// than the 0 every shader tested so far has had.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBufs[4] : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    float u = ((tid.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((tid.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
    q.Proceed();

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t = q.CommittedRayT();
        float2 b = q.CommittedTriangleBarycentrics();
        r.bx = b.x; r.by = b.y; r.hit = 1;
    }
    outBufs[2][tid.y * width + tid.x] = r;
}
