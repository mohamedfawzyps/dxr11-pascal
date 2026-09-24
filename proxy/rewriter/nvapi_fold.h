// NVAPI shader extension calls that ask a RayQuery for a cluster ID.
// C++ port of phase5/rewriter/nvapi.py, held byte-identical to it; the
// reasoning is written down there.
//
// In short: NVIDIA's HLSL extensions are stores to one UAV,
// RWStructuredBuffer<NvShaderExtnStruct>, which the driver reads as intrinsics
// while the application has registered that slot. Unreal registers it around
// every compute pipeline it creates with a vendor extension, so the shim's
// CreateStateObject runs with it registered, and a RayQuery cluster-ID call
// moved into a hit shader kills the driver (DRIVER_INTERNAL_ERROR). Ops 94 and
// 95 fold to 0xFFFFFFFF, the answer for every geometry when the device has no
// cluster operations, which the GTX 1070 reports; anything else is refused.
#pragma once

#include <string>

namespace rq {

// Runs on NORMALISED text, before analysis. Returns false with *why set when
// the module uses the extension UAV in a way this cannot fold. A module with
// no NvShaderExtnStruct UAV comes back unchanged.
bool FoldNvapi(const std::string& in, std::string* out, std::string* why);

}  // namespace rq
