// What does a SHADER MODEL 6.6 library look like, and how does it reach a
// resource?
//
// Unreal's RayQuery shaders are cs_6_6 and use the unified binding model:
// createHandleFromBinding (217) then annotateHandle (216), with ZERO uses of
// createHandle (57) or createHandleForLib (160). The rewriter was built against
// SM 6.5, where a library's handles come from createHandleForLib applied to a
// loaded global, and that difference is the root cause of 124 of Unreal's 157
// refusals:
//
//   118  the loop body "reads values defined outside it", because a handle made
//        with 217 is not on the recomputable list and so is not exempt
//     6  "Internal declaration 'rq_cbv0' is unused", because the rewriter
//        synthesises a global for every resource and nothing in a 6.6 module
//        ever loads one
//
// What to read off the output:
//   - whether a lib_6_6 uses createHandleFromBinding, or still createHandleForLib
//   - whether the !dx.resources records carry a real global or `undef`
//   - whether 216/217 are legal in an any-hit shader
//
//   dxc -T lib_6_6 phase5\cases\reference\lib_sm66_binding_ref.hlsl -Fc out.ll
cbuffer CB : register(b0) { float threshold; uint mode; };
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<float> outBuf : register(u0);

struct Payload { float t; uint hit; };

[shader("anyhit")]
void AnyHit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
    // A cbuffer read inside a hit shader, which is what the rebuilt chain has
    // to do.
    if (a.barycentrics.x < threshold) IgnoreHit();
    p.hit = mode;
}

[shader("closesthit")]
void ClosestHit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
    p.t = RayTCurrent() + threshold;
}

[shader("miss")]
void Miss(inout Payload p) { p.t = -1; p.hit = 0; }
