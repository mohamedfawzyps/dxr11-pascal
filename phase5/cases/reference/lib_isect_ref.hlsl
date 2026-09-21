// Reference only: what does a DXR 1.0 INTERSECTION shader compile to, and what
// is available inside one? Compile with -T lib_6_5.
struct ProcAttr { float pad; };
struct P { float t; uint hit; };
[shader("intersection")]
void Isect() {
    float3 o = ObjectRayOrigin();
    float3 d = ObjectRayDirection();
    ProcAttr a; a.pad = o.z + d.z;
    float tHit = 1.0 + a.pad;
    ReportHit(tHit, 0, a);
}
[shader("anyhit")]
void AHProc(inout P p, ProcAttr a) { p.t = a.pad; }
[shader("closesthit")]
void CHProc(inout P p, ProcAttr a) { p.t = RayTCurrent(); p.hit = 1; }
