// A Proceed loop that calls an NVAPI RayQuery extension, written the way
// NVIDIA's nvHLSLExtns.h writes every call: stores to one UAV of a 256-byte
// struct, which the driver reads as an intrinsic while the application has
// registered that UAV's slot. Unreal's shared TraceRayInline wrapper does this
// for the candidate and the committed cluster ID (ops 94 and 95), under a
// runtime flag, at u0 space1001; 47 MegaLights and Lumen shaders carry it.
//
// Not a render case: WARP knows nothing of NVAPI and would treat the calls as
// plain writes to an unbound buffer. It exists for the rewriter checks and for
// the driver check, which builds the lowered library with the slot registered
// as Unreal does. 0.41.0 transplanted the call into the any-hit shader and the
// driver died compiling it (DRIVER_INTERNAL_ERROR); 0.41.1 folds it to
// 0xFFFFFFFF, the answer for every geometry without cluster operations.
//
// The layout is NvShaderExtnStruct's, byte for byte, since the driver matches
// on it; the name is what the rewriter recognises.
//
// The driver check's root signature: dxc -T rootsig_1_1 -E RS on this file.
// The extension UAV has a counter, so it sits in a table.
#define RS "CBV(b0), SRV(t0), UAV(u0), DescriptorTable(UAV(u0, space=1001))"
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

struct NvShaderExtnStruct {
    uint  opcode;
    uint  rid;
    uint  sid;
    uint4 dst1u;
    uint4 src3u;
    uint4 src4u;
    uint4 src5u;
    uint4 src0u;
    uint4 src1u;
    uint4 src2u;
    uint4 dst0u;
    uint  markUavRef;
    uint  numOutputsForIncCounter;
    float padding1[27];
};
RWStructuredBuffer<NvShaderExtnStruct> g_NvidiaExt : register(u0, space1001);

uint ClusterIdCall(uint op, uint rqFlags) {
    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = op;
    g_NvidiaExt[index].src0u.x = rqFlags;
    return g_NvidiaExt.IncrementCounter();
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
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
            // 94, NV_EXTN_OP_RT_GET_CANDIDATE_CLUSTER_ID
            if (ClusterIdCall(94, q.RayFlags()) == 0xFFFFFFFF)
                q.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t  = q.CommittedRayT();
        // 95, NV_EXTN_OP_RT_GET_COMMITTED_CLUSTER_ID
        r.bx = (float)ClusterIdCall(95, q.RayFlags());
        r.by = q.CommittedTriangleBarycentrics().y;
        r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
