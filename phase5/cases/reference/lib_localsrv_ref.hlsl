// Reference: what DXC emits for a hit shader reading a RAW BUFFER bound at
// t0, space1, which is where a local root signature ROOT SRV would put it.
//
// The question it exists to answer: the Pascal driver crashes compiling a hit
// shader that reads a cbuffer through the LOCAL root signature (root constants
// or a root CBV, see phase5/cases/driver-crash/). A local root SRV is a buffer
// load, not a cbuffer load, and was never tested. If it is safe, each shader
// record can point at its own (geometry, contribution) pair and nothing needs
// baking. This file supplies the exact DXIL shape of the read, so the test
// edits the real crashing library to it instead of guessing.
//
// Compile: dxc -T lib_6_6 -Fc lib_localsrv_ref.ll lib_localsrv_ref.hlsl

struct Payload { uint geo; uint con; };

ByteAddressBuffer rec : register(t0, space1);

[shader("anyhit")]
void AnyHit(inout Payload p, in BuiltInTriangleIntersectionAttributes a) {
    uint2 v = rec.Load2(0);
    p.geo = v.x;
    p.con = v.y;
}
