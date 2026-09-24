// Values LOADED before the Proceed loop and used inside it, the shape of
// Unreal's MegaLights light sampling: it reads RWLightSamples[SampleCoord] and
// RWLightSampleRays[SampleCoord] at the top, tests shadows in the loop, and
// writes the same texel only after the trace. 24 MegaLights shaders were
// refused for this until 0.42.0, which has the generated hit shader read the
// value again.
//
// Both kinds of read are here:
//   pre  the thread's OWN output slot, a UAV, read before the trace and
//        written after it. raytest --prefill N fills the output buffer with a
//        pattern seeded by N first, on both sides, so the read returns data.
//   thr  a read-only buffer, alphaMask, an SRV.
// Both decide what the loop commits, so a hit shader reading either wrongly
// changes the image. The suite also runs WARP and the 1070 with DIFFERENT
// prefill seeds and requires DIVERGE, so the pre-read is shown to matter.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<float> alphaMask : register(t1);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    uint idx = tid.y * width + tid.x;
    uint pre = outBuf[idx].hit;
    float thr = alphaMask[(tid.x >> 3) & 7];

    float u = ((tid.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((tid.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            float2 bc = q.CandidateTriangleBarycentrics();
            uint s = ((uint)(bc.x * 8.0) + pre) & 7u;
            if ((alphaMask[s] > 0.5) != (thr > 0.5))
                q.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float2 cb = q.CommittedTriangleBarycentrics();
        r.t = q.CommittedRayT();
        r.bx = cb.x;
        r.by = cb.y;
        r.hit = 1;
    }
    outBuf[idx] = r;
}
