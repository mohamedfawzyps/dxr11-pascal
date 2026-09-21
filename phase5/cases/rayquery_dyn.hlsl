// The same resource array as rayquery_table.hlsl, indexed by a value the
// compiler cannot fold: DYNAMIC descriptor indexing, the last shape the
// rewriter refused for reasons of its own rather than reasons about DXR 1.0.
//
// The index is `width / 128`, which is 2 at runtime because --table binds the
// real output at slot 2 and decoys at 0, 1 and 3. It is a cbuffer load, so
// DXC cannot constant-fold it and emits a real getelementptr on the array
// global; write the 2 literally and this test becomes rayquery_table again and
// proves nothing.
//
// It is also deliberately UNIFORM. NonUniformResourceIndex is a separate
// thing, marked with !dx.nonuniform metadata on the getelementptr, and has its
// own case.
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
    uint pick = width / 128;
    outBufs[pick][tid.y * width + tid.x] = r;
}
