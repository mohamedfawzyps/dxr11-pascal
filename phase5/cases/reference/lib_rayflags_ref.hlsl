// What is RayFlags() in a DXR 1.0 hit shader, and does it cost a feature flag?
struct Payload { float t; uint hit; };
struct Attribs { float2 bary; };

[shader("anyhit")]
void AnyHit(inout Payload p, in Attribs a) {
    p.hit = RayFlags();
}

[shader("closesthit")]
void ClosestHit(inout Payload p, in Attribs a) {
    p.hit = RayFlags();
    p.t = RayTCurrent();
}

[shader("miss")]
void Miss(inout Payload p) { p.hit = RayFlags(); }
