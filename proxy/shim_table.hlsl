// The shader tables of a VARIANT pipeline, the shim's own record layout for
// GeometryIndex(). See proxy/geom_index_so.h.
//
// One thread per destination record. mode 0 copies record r of the
// application's table; mode 1 builds the shim's hit group table, where
//
//   r = c * span + instance * per + kc * g + (q % kc),   c = q / kc
//
// (q the TraceRay argument pair, g the geometry, c the scene copy's structure
// the pair is traced on, span = instances * per) and the record copied is the
// one the application's own layout would have used for that hit:
//
//   a = R[q] + M[q] * g + contribution[instance]
//
// Then, on the APPLICATION's identifier: an extended hit group gets g at its
// offset; and every identifier the variant pipeline changed is swapped for
// the variant's, with the shim's addresses written at that entry's offset
// when it has one (appended root descriptors: the scene copy's structures,
// and with arguments computed at run time the pair table, 0.57.0).
//
// Several scenes (0.59.0): the addresses are `perSlot` structures of each of
// `scenes` copies, then the table (`lut`). A record gets, for each of its
// `slots` scene slots, the structures of the copy its selection names:
// sel[selAt + a * selStride + slot], a the application's record (selStride 0:
// one selection for every record). The hit group table is built one scene at
// a time, each dispatch writing its scene's part of the table; r and the
// contributions are that scene's.
//
// A scene picked by key (0.60.0): `slots` counts SUB-SLOTS, a keyed slot
// holding several, each with its own selection; with `keys` the key table's
// address follows the pair table's.
//
//   meta, dwords: [0, 9 * groups)            identifier, offset
//                 [.., + 17 * remaps)        from identifier, to identifier, insert offset
//                 [.., + 2 * pairs)          R, M per pair
//                 [.., + 2 * addresses)      the addresses, low dword first
//                 [selAt, ...)               the selections
//
// The compiled form is proxy/shim_table_cs.h, checked in. Regenerate with:
//   C:\DW\DXC\bin\x64\dxc.exe -T cs_6_0 -E main -Vn g_shimTableCS
//       -Fh proxy\shim_table_cs.h proxy\shim_table.hlsl

#define RS "RootConstants(num32BitConstants=20, b0), SRV(t0), SRV(t1), SRV(t2), UAV(u0)"

cbuffer P : register(b0) {
    uint records; uint srcStride; uint dstStride; uint groups;
    uint remaps; uint mode; uint kc; uint gmax;
    uint appRecords; uint pairs; uint span; uint per;
    uint poison;   // DXR_TIER11_GI_POISON: write geometry 0, the sensitivity check
    uint slots; uint perSlot; uint scenes; uint lut;
    uint selAt; uint selStride; uint keys;
};
ByteAddressBuffer src : register(t0);
ByteAddressBuffer meta : register(t1);
ByteAddressBuffer contrib : register(t2);
RWByteAddressBuffer dst : register(u0);

bool Same(uint s, uint m) {
    bool same = true;
    for (uint w = 0; w < 8; ++w) same = same && src.Load(s + w * 4) == meta.Load(m + w * 4);
    return same;
}

[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint r : SV_DispatchThreadID) {
    if (r >= records) return;
    const uint d = r * dstStride;
    const uint pairAt = (groups * 9 + remaps * 17) * 4;
    uint a = r, g = 0xFFFFFFFFu;
    if (mode == 1) {
        // `per` is the scene copy's block per instance, kc * gmax of the
        // copy, which can be larger than this dispatch's needs: the rest of
        // the block is never reached and is zeroed.
        const uint c = r / span, rr = r % span;
        const uint rem = rr % per, q = c * kc + rem % kc;
        g = rem / kc;
        a = rem < kc * gmax && q < pairs
            ? meta.Load(pairAt + q * 8) + meta.Load(pairAt + q * 8 + 4) * g + contrib.Load((rr / per) * 4)
            : 0xFFFFFFFFu;
        if (a >= appRecords) {
            for (uint z = 0; z < dstStride; z += 4) dst.Store(d + z, 0u);
            return;
        }
    }
    const uint s = a * srcStride;
    for (uint b = 0; b < dstStride; b += 4)
        dst.Store(d + b, b < srcStride ? src.Load(s + b) : 0u);
    if (g != 0xFFFFFFFFu)
        for (uint x = 0; x < groups; ++x)
            if (Same(s, x * 36)) { dst.Store(d + meta.Load(x * 36 + 32), poison ? 0u : g); break; }
    const uint rb = groups * 36;
    const uint addrAt = pairAt + pairs * 8;
    for (uint y = 0; y < remaps; ++y) {
        const uint e = rb + y * 68;
        if (!Same(s, e)) continue;
        for (uint w = 0; w < 8; ++w) dst.Store(d + w * 4, meta.Load(e + 32 + w * 4));
        const uint at = meta.Load(e + 64);
        if (at) {
            for (uint j = 0; j < slots; ++j) {
                const uint sc = meta.Load((selAt + a * selStride + j) * 4);
                for (uint c = 0; c < perSlot; ++c) {
                    const uint from = addrAt + (sc * perSlot + c) * 8, to = d + at + (j * perSlot + c) * 8;
                    dst.Store(to, meta.Load(from));
                    dst.Store(to + 4, meta.Load(from + 4));
                }
            }
            for (uint x = 0; x < lut + keys; ++x) {
                const uint from = addrAt + (scenes * perSlot + x) * 8, to = d + at + (slots * perSlot + x) * 8;
                dst.Store(to, meta.Load(from));
                dst.Store(to + 4, meta.Load(from + 4));
            }
        }
        break;
    }
}
