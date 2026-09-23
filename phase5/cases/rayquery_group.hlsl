// SV_GroupID, SV_GroupThreadID and SV_GroupIndex, which a raygen does not have.
//
// The lowering rebuilds them from DispatchRaysIndex and numthreads, see
// _THREAD_OPS in phase5/rewriter/lower.py. Unreal's
// LumenRadianceCacheHardwareRayTracingCS reads SV_GroupID and SV_GroupIndex
// and 24 of one Escher run's refusals were exactly those, rejected by the
// validator in the raygen.
//
// Built so a wrong lowering cannot pass:
//   - The pixel is computed ONLY from SV_GroupID and SV_GroupThreadID, never
//     from SV_DispatchThreadID, so a wrong group value writes the wrong pixel.
//   - numthreads is 16 x 8, not square, so using the wrong axis's size for a
//     division, or swapping x and y, changes the answer.
//   - SV_GroupIndex GATES THE COMMIT inside the Proceed loop, so it is also
//     rebuilt in the generated any-hit shader, and a wrong flattening (y * 8
//     instead of y * 16, say) moves the stripes.
//
// The harness dispatches 32 x 32 groups, sized for 8 x 8; at 16 x 8 that is
// 512 x 256 threads, and the bounds check discards the overhang.

cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

[numthreads(16, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID,
          uint gi : SV_GroupIndex) {
    uint2 px = gid.xy * uint2(16, 8) + gtid.xy;
    if (px.x >= width || px.y >= height) return;
    float u = ((px.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((px.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        // A checker over the flattened group index, plus a corner of every
        // triangle, so both halves of the condition matter.
        //
        // Bit 4 XOR bit 2, and NOT bit 2 alone, which was the first version:
        // with gi = 16y + x, bit 2 comes only from x, so a lowering that
        // flattened with the wrong row stride (8y + x) matched WARP exactly.
        // Measured, and that is why this line changed. Bit 4 is y's lowest
        // bit under the right stride and something else under a wrong one.
        float2 b = q.CandidateTriangleBarycentrics();
        if ((((gi >> 4) ^ (gi >> 2)) & 1) == 0 || b.x + b.y < 0.5)
            q.CommitNonOpaqueTriangleHit();
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float2 b = q.CommittedTriangleBarycentrics();
        r.t = q.CommittedRayT();
        r.bx = b.x; r.by = b.y;
        r.hit = 1;
    }
    outBuf[px.y * width + px.x] = r;
}
