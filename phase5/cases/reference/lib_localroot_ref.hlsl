// What does a LOCAL root signature with root constants look like in a lowered
// library? Asked of DXC rather than reasoned about.
//
// This is the shape needed to answer four accessors the project had written off
// as impossible:
//
//   CandidateGeometryIndex                        (203)
//   CommittedGeometryIndex                        (209)
//   CandidateInstanceContributionToHitGroupIndex  (214)
//   CommittedInstanceContributionToHitGroupIndex  (215)
//
// A real Unreal 5.8 run refused 120 shaders on those four. The first two were
// called impossible because GeometryIndex() in a DXR 1.0 hit shader is ITSELF a
// Tier 1.1 feature; the second two because HLSL exposes no hit-shader intrinsic
// for them at all. Both reasons only hold while the APPLICATION owns the shader
// table. The shim builds the table, so it can put the answers in the record.
//
// With MultiplierForGeometryContributionToShaderIndex = 1, a hit lands on
// record (InstanceContribution + GeometryIndex), so the shim knows both numbers
// for every record it writes and can store them as root constants the hit
// shader reads back.
//
//   dxc -T lib_6_5 phase5\cases\reference\lib_localroot_ref.hlsl -Fc out.ll
//
// What to read off the output:
//   - how the cbuffer appears in !dx.resources and how it is bound
//   - which dx.op reads it (cbufferLoadLegacy) and with what handle
//   - whether the local root signature needs anything in the module at all, or
//     is purely a state-object subobject
//   - the SFI0 feature flags, because GeometryIndex already proved once that an
//     ordinary looking intrinsic can be secretly Tier 1.1

struct Payload {
    float t;
    uint  hit;
    uint  geometryIndex;
    uint  instanceContribution;
};

struct Attribs { float2 bary; };

// The local root signature's root constants, as the hit shaders see them.
// register(b0, space1) keeps them clear of anything the application's own
// global root signature binds, which the shim must not disturb.
cbuffer RecordConstants : register(b0, space1) {
    uint gRecordGeometryIndex;
    uint gRecordInstanceContribution;
};

[shader("anyhit")]
void AnyHit(inout Payload p, in Attribs a) {
    // The candidate forms: read directly, because the any-hit IS the Proceed
    // loop body and runs with this record's local arguments bound.
    if (gRecordGeometryIndex == 0xFFFFFFFF) IgnoreHit();
    p.geometryIndex = gRecordGeometryIndex;
    p.instanceContribution = gRecordInstanceContribution;
}

[shader("closesthit")]
void ClosestHit(inout Payload p, in Attribs a) {
    // The committed forms: travel back to the raygen in the payload, the same
    // way every other Committed accessor already does.
    p.t = RayTCurrent();
    p.hit = 1;
    p.geometryIndex = gRecordGeometryIndex;
    p.instanceContribution = gRecordInstanceContribution;
}

[shader("miss")]
void Miss(inout Payload p) {
    p.t = 0;
    p.hit = 0;
    p.geometryIndex = 0;
    p.instanceContribution = 0;
}
