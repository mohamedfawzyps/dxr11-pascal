// Detecting RayQuery in a DXIL container, without parsing any bitcode.
//
// The brief is explicit that until the rewriter runs inside the proxy, a
// RayQuery shader must be detected and failed CLEARLY rather than passed to a
// driver that cannot run it. That needs a cheap, reliable test.
//
// It turns out to be nearly free. MEASURED across every shader in phase5:
//
//     plain compute shader        SFI0 = 0x0
//     every RayQuery shader       SFI0 = 0x100000
//     DXC-built DXR 1.0 library   SFI0 = 0x0
//     the rewriter's own output   SFI0 = 0x0
//
// SFI0 is the container's Shader Feature Info part, eight bytes of flags, and
// bit 20 is the Tier 1.1 feature bit. So the test is a container walk and a
// mask, with no LLVM involved at all.
#pragma once

#include <windows.h>

// Bit 20 of the SFI0 feature flags: raytracing tier 1.1 features, which is
// what using RayQuery sets.
const UINT64 kDxilFeatureRaytracingTier11 = 1ull << 20;

// The SFI0 flags of a DXIL container, or 0 if it has no SFI0 part or does not
// look like a container at all. A malformed blob reads as 0 rather than
// throwing: this runs on application-supplied memory in a hot path, and
// guessing wrong must not take the process down.
UINT64 Dxr11ContainerFeatureFlags(const void* data, SIZE_T size);

// Convenience: does this shader use RayQuery?
bool Dxr11ContainerUsesRayQuery(const void* data, SIZE_T size);
