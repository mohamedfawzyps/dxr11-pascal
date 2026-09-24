// The instance descriptions of the shim's copy of an application's top-level
// structure. See proxy/shim_scene.h.
//
// One thread per instance. The description is copied from the application's
// buffer, by ADDRESS through a root SRV (the build it was given to requires it
// in NON_PIXEL_SHADER_RESOURCE, the state this read needs), with
// InstanceContributionToHitGroupIndex (the low 24 bits of dword 13) replaced
// by instance * stride: every instance gets a block of records of its own,
// one per (geometry, TraceRay argument pair). The application's contribution
// goes to `contrib`, because the shim's hit group table is filled from the
// application's records and needs it to find them.
//
// The compiled form is proxy/shim_scene_cs.h, checked in. Regenerate with:
//   C:\DW\DXC\bin\x64\dxc.exe -T cs_6_0 -E main -Vn g_shimSceneCS
//       -Fh proxy\shim_scene_cs.h proxy\shim_scene.hlsl

#define RS "RootConstants(num32BitConstants=2, b0), SRV(t0), UAV(u0), UAV(u1)"

cbuffer C : register(b0) { uint count; uint stride; };
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
RWByteAddressBuffer contrib : register(u1);

[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID) {
    if (i >= count) return;
    for (uint w = 0; w < 16; ++w) {
        uint v = src.Load(i * 64 + w * 4);
        if (w == 13) {
            contrib.Store(i * 4, v & 0xFFFFFFu);
            v = (v & 0xFF000000u) | ((i * stride) & 0xFFFFFFu);
        }
        dst.Store(i * 64 + w * 4, v);
    }
}
