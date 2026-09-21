# Using the shim

Step by step, from a clean checkout to a running application.

## 1. What you need

- Windows x64 and a GPU that reports `D3D12_RAYTRACING_TIER_1_0`. NVIDIA
  Pascal (GTX 10-series) or Turing GTX 16-series, with a driver from 425.31
  (April 2019) or newer
- MSVC. The build scripts find it through `vswhere`, so any terminal works
- The [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)
  and the [D3D12 Agility SDK](https://devblogs.microsoft.com/directx/directx12agility/)

Nothing from NVIDIA or Microsoft ships in this repository. You supply DXC and
the Agility SDK yourself, and the paths are set at the top of the build
scripts. The defaults expect them at `C:\DW\DXC` and
`C:\DW\microsoft.direct3d.d3d12.1.619.5`.

## 2. Build

```
build_proxy.bat
```

That produces `d3d12.dll` in the repository root. It is the whole shim: the
DXIL rewriter and its DXC host are linked in, so there is nothing else to
deploy.

## 3. Install it next to the application

A proxy DLL works by sitting in the same directory as the executable, so
Windows loads it before the real `d3d12.dll` in `System32`.

```
copy d3d12.dll "<directory containing the .exe>"
```

**It must be beside the .exe itself**, not beside a launcher and not in the
game or project root. For Unreal that usually means:

| What you are running | Where the .exe is |
|---|---|
| Unreal Editor | `Engine\Binaries\Win64\UnrealEditor.exe` |
| A packaged game | `<Project>\Binaries\Win64\<Project>.exe` |

DXC is loaded at runtime, by full path, from beside the shim. If the
application turns out to need it (it only loads when a RayQuery shader
actually arrives), copy `dxcompiler.dll` and `dxil.dll` into the same
directory. The shim deliberately never loads them by name, because the
application may already have its own copy of a different version loaded.

## 4. Turn it on

```
set DXR11_TIER11=1
```

Without this the shim reports Tier 1.0 and forwards everything unchanged,
which is the safe default. With it, `CheckFeatureSupport` reports Tier 1.1 and
RayQuery shaders are rewritten.

**The variable has to be set in the environment that launches the .exe.** A
`set` in one terminal does not reach a program started from Explorer or from
the Epic Games Launcher. Three ways, in order of preference:

Launch from the same terminal:

```
set DXR11_TIER11=1
"Engine\Binaries\Win64\UnrealEditor.exe"
```

Or write a one-line launcher next to the .exe:

```bat
@echo off
set DXR11_TIER11=1
start "" "%~dp0UnrealEditor.exe" %*
```

Or set it for your whole user account, which survives reboots and reaches
launchers. Remember you have set it:

```
setx DXR11_TIER11 1
```

## 5. Read the log

Every run writes `%TEMP%\dxr11_proxy.log`. Open it first, always. It is the
only thing that tells you what the shim actually did.

```
type %TEMP%\dxr11_proxy.log
```

A healthy start looks like this:

```
[dxr11-proxy] attached to process, version 0.9.0
[dxr11-proxy] device wrapping enabled
[dxr11-proxy] device wrapper created (real=..., Device6=yes, Device7=yes, tier=1.0)
[dxr11-proxy] DXR11_TIER11=1: reporting Tier 1.1 to the application.
[dxr11-proxy] queue hook installed: vtable ... slot 10, self-test passed
```

The first line proves the shim loaded at all, and says which build. If the
file does not exist, the DLL is in the wrong directory and nothing else in
this guide matters.

Then, when a RayQuery shader arrives:

```
[dxr11-proxy] CreateComputePipelineState: shader USES RAYQUERY (SFI0 bit 20), 4812 bytes.
[dxr11-proxy] RayQuery compute shader lowered and ready: 4812 -> 8948 bytes, numthreads(8,8,1)
```

or, when it cannot be lowered:

```
[dxr11-proxy] RayQuery compute shader NOT lowered: <the reason>
```

A refused shader is forwarded unchanged, so the driver reports its own error
rather than the shim inventing one. The reason line is the useful part.

## 6. Check it is actually working

Before trusting it on a real application, run the suites. They take ground
truth from WARP on every run, so they test your build on your machine rather
than comparing against a stored baseline.

```
tools\run_dispatch_test.ps1     RayQuery shaders end to end through the proxy
tools\run_rewriter_test.ps1     the rewriter, and Python/C++ agreement
```

Both should end with every case passing and both refusal gates behaving.

## 7. When something goes wrong

Work down this list in order. Each step rules out a layer.

**No log file at all.** The DLL is not beside the .exe, or the application
loads D3D12 from somewhere else. Confirm the .exe path, and confirm you copied
to that directory and not to the project root.

**The log exists but stops after "attached to process".** The application
never created a D3D12 device. Something failed earlier, unrelated to the shim.

**Is the shim the problem at all?** Set `DXR11_NO_WRAP=1`. That disables
device wrapping and restores plain forwarding while leaving the DLL in place.
If the symptom persists, it is not the shim's translation. If it clears, it
is, and the log says what was being translated when it happened.

**Rendering is wrong rather than broken.** Search the log for `REFUSED` and
for `NOTE:`. The shim refuses what it cannot serve, and it also warns about
scene layouts it can only partly serve.

**A shader is refused.** The reason is in the log, in full. The permanent ones
are in the README; those will not change, because DXR 1.0 has nothing to lower
them onto.

**Turn the debug layer on.** `DXR11_DEBUGLAYER=1` for the test harness. The
D3D12 debug layer reports through `OutputDebugString`, so a console shows
nothing without a drain. Note that it does not police everything: it is silent
on hit group and geometry type mismatches, so its silence is not evidence on
its own.

## 8. Turning it off

Three levels, increasing in thoroughness:

| Goal | Do this |
|---|---|
| Report Tier 1.0 again, keep the shim loaded | unset `DXR11_TIER11` |
| Forward everything, no wrapping | `set DXR11_NO_WRAP=1` |
| Remove the shim entirely | delete `d3d12.dll` from the .exe's directory |

Deleting the DLL always restores the original behaviour exactly. The shim adds
no registry keys, no services and no files outside its own directory and the
log in `%TEMP%`.

## 9. Notes for Unreal

Unreal ships its own Agility SDK, which redirects D3D12 to a `D3D12Core.dll`
in a subdirectory. That works with this shim: the proxy exports the three
undocumented `D3D12Core*` entry points that `d3d12SDKLayers.dll` imports, so
the Agility runtime and the debug layer both still load.

Coverage against Unreal is a **source survey, not an execution test**. Epic's
RayQuery use is all in compute shaders, which is the shape this shim handles,
and 21 of the 25 accessors Epic uses are supported. The other four cannot be
lowered, and Epic does use them, so expect some shaders to be refused. The log
will name them.
