// Reference only: a compute shader with NO RayQuery, for comparing feature bits.
RWStructuredBuffer<float> outBuf : register(u0);
[numthreads(8,8,1)]
void main(uint3 tid : SV_DispatchThreadID) { outBuf[tid.x] = tid.y; }
