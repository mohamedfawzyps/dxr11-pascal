// Reference only: what does a hit group that must NEVER commit compile to?
// Compile with -T lib_6_5.
//
// The per-index typed table needs two of these. A scene can route triangle
// geometry and procedural geometry to DIFFERENT hit group records, and the
// shim has a real hit group for only ONE of those kinds. The other index needs
// a record of the right TYPE that produces no hit, so the geometry the lowered
// shader does not handle is simply not seen, which is what the shader would do
// on Tier 1.1 anyway.
//
// Two questions for DXC: what IR does each stub become, and does either of
// them pull in a shader feature flag that Tier 1.0 refuses? Check SFI0 as well
// as the disassembly, the way lib_accessors_ref did, because GeometryIndex
// already proved an intrinsic can look ordinary and be Tier 1.1.
struct P { float t; uint hit; };
struct ProcAttr { float pad; };

// For a TRIANGLES hit group. Every candidate is rejected, so nothing is ever
// committed and traversal carries on past this geometry.
[shader("anyhit")]
void AnyHitNull(inout P p, BuiltInTriangleIntersectionAttributes attr) {
    IgnoreHit();
}

// For a PROCEDURAL hit group. Reporting nothing IS generating no hit: an
// intersection shader that returns has found nothing.
[shader("intersection")]
void IsectNull() {
}
