// Reference only: the shape of a hit shader reading one 32-bit constant from
// a cbuffer in a register space no application uses, which is how the shim
// hands GeometryIndex() to an application's own hit shader. lib_6_5 and 6_6.
cbuffer DxrShimGeometry : register(b0, space2147420000) { uint ShimGeometryIndex; };
struct Payload { uint a; uint b; };
[shader("closesthit")]
void CH(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {
    p.a = ShimGeometryIndex;
    p.b = PrimitiveIndex();
}
