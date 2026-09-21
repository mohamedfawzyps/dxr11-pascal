# Phase 3: proxy d3d12.dll - notes

Goal: a proxy `d3d12.dll` that an app loads instead of the system one, forwards
everything to the real runtime, and (later) wraps the device so we can spoof the
raytracing tier and intercept calls. No translation in this phase.

## Why a proxy DLL works

Windows searches the exe's own directory before System32 for a DLL loaded by
name. Dropping our `d3d12.dll` beside an app's exe makes the app bind to us. We
load the real runtime by full System32 path, so forwarding never recurses.

## Staged plan

- 3a (done): forwarding-only. Exports the d3d12 entry points, forwards each to
  the real d3d12.dll, and logs D3D12CreateDevice to confirm the app runs through
  us. Fully transparent, no device wrapping.  Source: `proxy/d3d12_proxy.cpp`,
  exports in `proxy/d3d12_proxy.def`.
- 3b (next): wrap the returned `ID3D12Device5` (and the interfaces it hands out
  that we will later intercept), forwarding every method unchanged, and
  re-verify the sample. This is the seat for Phase 4/5 (tier spoof,
  CreateStateObject, command-list interception).

## Build (Windows x64)

    build_proxy.bat            -> d3d12.dll

## Validate against a Microsoft DXR sample

1. Get microsoft/DirectX-Graphics-Samples and build a DXR 1.0 sample, e.g.
   `Samples/Desktop/D3D12Raytracing/src/D3D12RaytracingSimpleLighting`
   (or HelloWorld / ProceduralGeometry), x64.
2. Copy our `d3d12.dll` into the sample's output folder, next to its `.exe`.
3. Run the sample. It should render exactly as before.
4. Check `%TEMP%\dxr11_proxy.log`: it should contain the "attached to process"
   line and a `D3D12CreateDevice ... hr=0x00000000` line, proving the app went
   through the proxy.

To confirm the proxy is really the one being used, temporarily rename it and the
sample should still run (using the system dll); restore it and the log should
reappear.

## Status

- 3a written; not yet built/validated on the dev machine. Forwarders are thin
  and the export set covers what the MS DXR samples import; if the sample fails
  to load, the missing export name will name itself and we add it.

## Notes

- We only proxy d3d12.dll. dxgi.dll is left to the system.
- PIX exports are not forwarded; the MS samples do not import them. Add them if a
  target app needs them.
