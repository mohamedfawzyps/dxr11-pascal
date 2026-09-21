// A second independent shader, using committed accessors the whitelist did
// not cover: CommittedInstanceIndex and CommittedPrimitiveIndex. These are
// common in real code, where a hit is only the beginning and the indices are
// what you look material data up with.
//
// CommittedGeometryIndex is deliberately NOT here. It has no lowering on Tier
// 1.0: GeometryIndex() in a DXR 1.0 hit shader is itself a Tier 1.1 feature,
// and CreateStateObject on the GTX 1070 refuses a library that uses it. The
// rewriter says so rather than emitting something the driver rejects, and
// test_reject.py covers that path.
//
// The three indices are packed into the Result's float fields so the existing
// diff verifies them: WARP and the lowered version must agree on all three.
// Needs --multi, a scene where all three actually vary; against the default
// one-triangle scene every correct answer is 0 and the test proves nothing.
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

    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
    q.Proceed();

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t  = (float)q.CommittedInstanceIndex();
        r.bx = (float)q.CommittedPrimitiveIndex();
        r.by = q.CommittedRayT();
        r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
