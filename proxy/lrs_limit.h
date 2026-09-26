// How large a LOCAL root signature the GTX 1070's driver takes. One past it
// REMOVES THE DEVICE inside CreateRootSignature (DXGI_ERROR_DRIVER_INTERNAL_
// ERROR); WARP took 520 root descriptors. Measured with tier11/lrsprobe.cpp
// (0.59.0, and the layout rule 0.63.0), every mode one run each:
//
// Walk the parameters in order, costing constants 1 dword each, a root
// descriptor 2 (at an even offset), a descriptor table 1. If a constants
// parameter ENDS past dword 64, the total may be at most that parameter's
// start + 128; otherwise at most 192.
//
//   root SRVs only                        96        (192)
//   one constants parameter               128       (crosses at 0: 0 + 128)
//   c constants, then root SRVs           1:95 16:88 32:80 64:64 (192)
//                                         65:31 80:24 100:14 127:0 (0 + 128)
//   c split over two parameters           32:80 64:64 (192), 80:44 (40 + 128),
//                                         100:39 (50 + 128)
//   descriptor tables only                192
//   32 constants, 40 root SRVs, tables    80        (192)
//   64 constants, 63 root SRVs, a table   of up to 33280 descriptors (191)
//   k root SRVs, c constants, then n:     1,64 SRVs 32 (2 + 128); 1,64 tables
//                                         64 (2 + 128); 4,56 SRVs 64 (192);
//                                         5,56 SRVs 36 (10 + 128); 2,64 SRVs
//                                         32 (4 + 128); 1,65 SRVs 31 (2 + 128);
//                                         0,50 tables 142 (192); 3,20 SRVs 83
//
// The rule until 0.63.0 counted totals only (at most 64 constants: 192), so
// a root descriptor BEFORE 64 constants passed at 191 dwords where the driver
// dies past 130: it could extend a signature into a device removal.
#pragma once

#include <d3d12.h>

#include <vector>

namespace lrslimit {

// Does the driver take a local root signature of these parameters?
inline bool Fits(const D3D12_ROOT_PARAMETER1* p, UINT n, UINT* used = nullptr, UINT* limit = nullptr) {
    UINT o = 0, cap = 192;
    bool crossed = false;
    for (UINT i = 0; i < n; ++i) {
        switch (p[i].ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            const UINT c = p[i].Constants.Num32BitValues;
            if (!crossed && o + c > 64) { crossed = true; cap = o + 128; }
            o += c;
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            o += 1;
            break;
        default:
            o = (o + 1) / 2 * 2 + 2;
            break;
        }
    }
    if (used) *used = o;
    if (limit) *limit = cap;
    return o <= cap;
}
inline bool Fits(const std::vector<D3D12_ROOT_PARAMETER1>& p, UINT* used = nullptr, UINT* limit = nullptr) {
    return Fits(p.data(), (UINT)p.size(), used, limit);
}

}  // namespace lrslimit
