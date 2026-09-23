// A COPY of rayquery_geom.hlsl, compiled at cs_6_6 because of this file's
// name (build_phase5.bat, and raytest's --cs). A copy and not an #include
// because raytest compiles from memory with no include handler; keep the two
// in step. The shader model is the test: at 6.6 the record handle has to be
// ANNOTATED, and a bare createHandleForLib on a local root signature resource
// is what crashed the Pascal driver.

// GeometryIndex, the accessor this project called impossible for longest.
//
// A real Unreal 5.8 run refused 105 shaders on CandidateGeometryIndex and 3 on
// CommittedGeometryIndex, more than everything else put together. Falcor uses
// it to identify every hit. It is how a renderer looks up which material it
// just hit, so a shim that cannot serve it cannot serve a renderer.
//
// It was written off because GeometryIndex() in a DXR 1.0 hit shader is ITSELF
// a Tier 1.1 feature: it sets shader flag 0x2000000 and CreateStateObject
// rejects the library on a GTX 1070. That reasoning was sound and is now
// obsolete, because it assumed the application owns the shader table. The shim
// builds the table, so the geometry index travels in the record as a local root
// signature constant and the hit shader reads it back. Measured at SFI0 = 0x0,
// see phase5/cases/reference/lib_localroot_ref.hlsl.
//
// Needs --geom, a scene whose one bottom-level structure holds FOUR geometries,
// one quad per quadrant. Against every other scene here the right answer is 0
// everywhere and a lowering that dropped the index entirely would pass.
//
// Both forms are read on purpose. They take different routes: the candidate
// form is read in the generated any-hit, which runs with the record's own
// constants bound, while the committed form is read in the generated
// closest-hit and travels home in the payload.
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

    // NOT forced opaque, so Proceed yields a candidate and the CANDIDATE form
    // is actually exercised. With RAY_FLAG_FORCE_OPAQUE there is no any-hit at
    // all and half of what this tests would never run.
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
