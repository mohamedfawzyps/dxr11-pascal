// Copies `dwords` 4-byte words from an application buffer, by ADDRESS through
// a root SRV, into the shim's own buffer: a raygen shader record in GPU
// memory, read so the scene its local root signature carries can be found at
// submit (0.55.0). A shader table must be in NON_PIXEL_SHADER_RESOURCE to be
// dispatched, the state this read needs, and the shim never names the
// application's resource (the rule since 0.39.2).
//
// The compiled form is proxy/shim_copy_cs.h, checked in. Regenerate with:
//   C:\DW\DXC\bin\x64\dxc.exe -T cs_6_0 -E main -Vn g_shimCopyCS
//       -Fh proxy\shim_copy_cs.h proxy\shim_copy.hlsl

#define RS "RootConstants(num32BitConstants=1, b0), SRV(t0), UAV(u0)"

cbuffer C : register(b0) { uint dwords; };
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);

[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID) {
    if (i < dwords) dst.Store(i * 4, src.Load(i * 4));
}
