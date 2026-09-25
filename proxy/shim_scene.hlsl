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
// With `verbatim` set it copies the descriptions unchanged and writes no
// contributions: the shim's SAVED copy of a build's GPU-written instances,
// which the scene copy is built from later (0.51.0).
//
// `copies` scene copies are written one after another, copy c's
// contributions offset by c * count * stride: more than 15 TraceRay argument
// pairs need more than one, since a multiplier has 4 bits (0.57.0). All of
// them start at `base`, after another scene's records (0.59.0).
//
// The compiled form is proxy/shim_scene_cs.h, checked in. Regenerate with:
//   C:\DW\DXC\bin\x64\dxc.exe -T cs_6_0 -E main -Vn g_shimSceneCS
//       -Fh proxy\shim_scene_cs.h proxy\shim_scene.hlsl

#define RS "RootConstants(num32BitConstants=5, b0), SRV(t0), UAV(u0), UAV(u1)"

cbuffer C : register(b0) { uint count; uint stride; uint verbatim; uint copies; uint base; };
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
RWByteAddressBuffer contrib : register(u1);

[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID) {
    if (i >= count * copies) return;
    const uint c = i / count, j = i % count;
    for (uint w = 0; w < 16; ++w) {
        uint v = src.Load(j * 64 + w * 4);
        if (w == 13 && !verbatim) {
            if (c == 0) contrib.Store(j * 4, v & 0xFFFFFFu);
            v = (v & 0xFF000000u) | ((base + c * count * stride + j * stride) & 0xFFFFFFu);
        }
        dst.Store(i * 64 + w * 4, v);
    }
}
