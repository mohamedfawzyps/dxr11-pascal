// Reads the group counts of an indirect compute dispatch WITHOUT touching the
// state of the application's argument buffer. See proxy/group_count.h.
//
// The shim replays the application's own ExecuteIndirect with this shader
// bound. Every group records SV_GroupID + 1 with an atomic max, so the buffer
// ends up holding exactly the X, Y and Z the GPU dispatched, or zeros if any
// of them was zero and no group ran. Only the groups on each axis write, one
// atomic each, so the cost is X + Y + Z atomics however large the grid.
//
// The compiled form is proxy/group_count_cs.h, checked in. Regenerate with:
//   C:\DW\DXC\bin\x64\dxc.exe -T cs_6_0 -E main -Vn g_groupCountCS
//       -Fh proxy\group_count_cs.h proxy\group_count.hlsl

#define RS "UAV(u0)"

RWByteAddressBuffer counts : register(u0);

[RootSignature(RS)]
[numthreads(1, 1, 1)]
void main(uint3 gid : SV_GroupID) {
    uint unused;
    if (gid.y == 0 && gid.z == 0) counts.InterlockedMax(0, gid.x + 1, unused);
    if (gid.x == 0 && gid.z == 0) counts.InterlockedMax(4, gid.y + 1, unused);
    if (gid.x == 0 && gid.y == 0) counts.InterlockedMax(8, gid.z + 1, unused);
}
