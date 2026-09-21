// Reference only: what does DXC emit for a resource ARRAY in a library?
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBufs[4] : register(u0);
[shader("raygeneration")]
void RayGen() {
    Result r = (Result)0; r.hit = 1;
    outBufs[2][DispatchRaysIndex().x] = r;
}
