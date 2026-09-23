// Copies a top-level build's instance descriptions out of the application's
// memory WITHOUT naming the application's resource. See proxy/group_count.h.
//
// The source is a ROOT SRV, which takes a bare GPU virtual address: exactly
// what the build call was given, so there is no resource to look up and none
// to transition. DXR requires the instance buffer to be in
// NON_PIXEL_SHADER_RESOURCE at the build, which is the state a compute shader
// read needs, so the read is legal wherever the build is.
//
// Up to 0.39.1 the shim looked the address up in its resource tracker, which
// holds no references, and recorded a barrier and a copy on whatever it found.
// During a level load that can be a resource the application already freed,
// and a list referencing a deleted resource fails Close with E_INVALIDARG.
//
// The compiled form is proxy/instance_copy_cs.h, checked in. Regenerate with:
//   C:\DW\DXC\bin\x64\dxc.exe -T cs_6_0 -E main -Vn g_instanceCopyCS
//       -Fh proxy\instance_copy_cs.h proxy\instance_copy.hlsl

#define RS "RootConstants(num32BitConstants=1, b0), SRV(t0), UAV(u0)"

cbuffer Count : register(b0) { uint dwords; };
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);

[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID) {
    if (id < dwords) dst.Store(id * 4, src.Load(id * 4));
}
