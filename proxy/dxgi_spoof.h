// Reporting a Pascal card under a Turing device id, because Unreal Engine
// refuses ray tracing on Pascal by PCI DEVICE ID and nothing else.
//
// Everything else in this project translates an API shape. This does not: it
// changes an answer. That deserves stating plainly, along with why there is no
// alternative and why the lie is as small as it is.
//
// WHY. Engine/Source/Runtime/D3D12RHI/Private/Windows/WindowsD3D12Device.cpp,
// verified in stock UE 5.7.4:
//
//     static bool IsRayTracingEmulated(uint32 DeviceId)
//     { ... 0x1B81, // "NVIDIA GeForce GTX 1070" ... }
//
//     if (GRHISupportsRayTracing && IsRayTracingEmulated(AdapterDesc.DeviceId))
//     {
//         DisableRayTracingSupport();
//         UE_LOG(..., TEXT("Ray tracing is disabled for NVIDIA cards with the Pascal architecture."));
//     }
//
// It runs AFTER the tier check. So the shim reports Tier 1.1, Unreal believes
// it, sets GRHISupportsRayTracing, then reads the device id and turns it all
// back off. No cvar, no command line flag and no config guards it: the
// GAllowEmulatedRayTracing escape hatch that older engine versions had was
// removed. Measured end to end: a whole session with the tier accepted and not
// one acceleration structure built.
//
// THE LIE IS ONE FIELD. DeviceId, and only on adapters that are on Unreal's
// own list. Not the vendor id, so NVAPI still runs and still answers honestly.
// Not the description string, because Windows' own driver lookup keys on it
// (FWindowsPlatformMisc::GetGPUDriverInfo takes the description and matches it
// against EnumDisplayDevices), and lying there breaks a real lookup and gains
// nothing.
//
// WHAT STAYS TRUE. Everything that is a capability question. Unreal asks NVAPI
// for shader execution reordering, cluster operations, 64-bit atomics and the
// driver version, and every one of those goes to the real device and comes
// back with the real answer. Shader execution reordering correctly reports
// unsupported. That is the same principle as the rest of this project: never
// claim a capability, only remove a gate that was keyed on a name.
//
// HOW. dxgi.dll gives every adapter object the same vtable, and it lives in
// the module image rather than on the heap. Measured, because this project has
// been wrong about that before: command QUEUES share an image vtable and
// command LISTS get per-object heap ones. Adapters share, so four patched
// slots cover every adapter from every factory for the life of the process,
// and no adapter is ever wrapped. Nothing downstream can tell the difference,
// including D3D12CreateDevice, which keeps receiving the real object.
#pragma once

#include <windows.h>

namespace spoof {

// Patch IDXGIAdapter::GetDesc, GetDesc1, GetDesc2 and GetDesc3 in the shared
// vtable. Called once, from the first CreateDXGIFactory*, which is the earliest
// moment an adapter can exist and is before any application reads a
// description. Idempotent and safe to call again.
//
// Self-tests every slot it patches and rolls ALL of them back if any one does
// not behave, so a wrong slot index cannot be left in place. Does nothing when
// the setting is off, or when no adapter needs it.
void InstallAdapterHook();

}  // namespace spoof
