// Builds the shim's copy of an application's hit group table, with the
// geometry index each record answers written into it. See proxy/geom_index_so.h.
//
// One thread per record. The record is copied from the application's table,
// by ADDRESS through a root SRV (DXR already requires a shader table to be in
// NON_PIXEL_SHADER_RESOURCE, the state this read needs), into the shim's
// buffer at the shim's stride, the tail zeroed. If its shader identifier is
// one of the hit groups whose local root signature the shim extended, that
// record's geometry index goes at the offset the extension put it.
//
//   meta, dwords:  [0, 9 * groups)      per group: 8 identifier dwords, offset
//                  [9 * groups, + recs) per record: its geometry index
//
// The compiled form is proxy/geom_table_cs.h, checked in. Regenerate with:
//   C:\DW\DXC\bin\x64\dxc.exe -T cs_6_0 -E main -Vn g_geomTableCS
//       -Fh proxy\geom_table_cs.h proxy\geom_table.hlsl

#define RS "RootConstants(num32BitConstants=4, b0), SRV(t0), SRV(t1), UAV(u0)"

cbuffer P : register(b0) { uint records; uint srcStride; uint dstStride; uint groups; };
ByteAddressBuffer src : register(t0);
ByteAddressBuffer meta : register(t1);
RWByteAddressBuffer dst : register(u0);

[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint r : SV_DispatchThreadID) {
    if (r >= records) return;
    const uint s = r * srcStride, d = r * dstStride;
    for (uint b = 0; b < dstStride; b += 4)
        dst.Store(d + b, b < srcStride ? src.Load(s + b) : 0u);
    for (uint k = 0; k < groups; ++k) {
        bool same = true;
        for (uint w = 0; w < 8; ++w)
            same = same && src.Load(s + w * 4) == meta.Load((k * 9 + w) * 4);
        if (same) {
            dst.Store(d + meta.Load((k * 9 + 8) * 4), meta.Load((groups * 9 + r) * 4));
            break;
        }
    }
}
