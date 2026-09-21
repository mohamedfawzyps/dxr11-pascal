// Reference only: which dx.op opcodes are InstanceIndex, PrimitiveIndex and
// GeometryIndex in a DXR 1.0 closest-hit shader? Compile with -T lib_6_5.
struct Payload { uint a; uint b; uint c; };
[shader("closesthit")]
void CH(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
    p.a = InstanceIndex();
    p.b = PrimitiveIndex();
    p.c = GeometryIndex();
}
