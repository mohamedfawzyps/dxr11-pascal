# Using the shim

Step by step, from a clean checkout to a running application.

![How the shim sits between an application and the real D3D12 runtime](shim-overview.svg)

Worth understanding before anything else: the shim is a DLL that sits beside
the application's .exe, because Windows searches that directory before
`System32`. Nothing is installed and nothing in `System32` is touched. The real
runtime, the driver and the GPU are all untouched, and the GPU still does every
ray.

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

## The short way

`tools\dxr-tier-11-setup.bat` does steps 2 to 4 for you: pick the .exe, click
Install, and the settings and the log are on the same window. It copies files
and writes a two line .ini, nothing else, and it is a readable PowerShell
script if you want to check that before running it.

The rest of this page is the same thing by hand, and is worth reading when
something does not work.

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

### Which files go in that directory

| File | Needed | Why |
|---|---|---|
| `d3d12.dll` | always | the shim itself |
| `dxcompiler.dll` | for RayQuery | rewrites the shader |
| `dxil.dll` | for RayQuery | signs the result, nothing runs unsigned |

Those are the only three. You do **not** need `D3D12Core.dll`, which an
application using the Agility SDK ships for itself, and you do **not** need
`d3d12SDKLayers.dll`, which is only for the debug layer and comes from the
Graphics Tools optional Windows feature.

The two DXC files are loaded **lazily**, only when a RayQuery shader actually
arrives, and by FULL PATH from beside the shim. A plain DXR 1.0 application
never touches them. The full path matters: a bare load by name would find the
application's own copy of DXC, of whatever version, which is not the one this
was built against.

**Without them, Tier 1.1 is not reported at all.** Claiming Tier 1.1 is a
promise to translate RayQuery, and that promise cannot be kept without DXC. So
the shim checks, and says so rather than letting an application discover it at
the first shader:

```
[dxr-tier-11-proxy-log] NOT reporting Tier 1.1: dxcompiler.dll is not next to the shim
... Tier 1.0 is reported instead, which is honest, and the application will
simply not use inline ray tracing.
```

The application then behaves exactly as it would with no shim present, which is
a working program without ray tracing rather than a crash.

### Where to get DXC

Download a release from Microsoft and copy `dxcompiler.dll` and `dxil.dll`
out of `bin\x64\`:

- https://github.com/microsoft/DirectXShaderCompiler/releases

They are not redistributed here on purpose. `dxil.dll` in particular is a
Microsoft binary with its own terms, so you get it from Microsoft.

**Which version.** This was built and tested against DXC 1.10.2605.37, the
`dxc_2026_08_11` release. A newer one should work and is the better default:
DXIL is pinned to LLVM 3.7 bitcode so the text format does not move between
versions, Microsoft only ever appends `dx.op` opcode numbers rather than
renumbering them, and the rewriter refuses any opcode it does not recognise
rather than guessing, so a newer compiler produces a clean refusal in the
worst case and not a wrong shader.

An OLDER one may not work. `dxil.dll` cannot sign a shader model newer than
itself, and a mismatched `dxcompiler.dll` and `dxil.dll` pair can fail
validation. Take both files from the same release.

**Only one version has actually been tested.** If something fails at signing
or validation, the version you used is the first thing to say in a bug report.

## 4. It is already on

There is no fourth step. Tier 1.1 is reported by default, because putting the
DLL there was the opt-in. Launch the application however you normally do, from
Steam, from the Epic launcher, from Explorer. Nothing needs a terminal.

### Turning it off, or changing anything

Copy `dxr11.example.ini` into the same directory, rename it to `dxr11.ini` and
edit it. A file beside the DLL is the only mechanism that works regardless of
how the application is launched, since an environment variable set in a console
never reaches a game started by a launcher.

```ini
tier11 = 0
```

Environment variables of the same name still work and take priority over the
file, which is what the test scripts use:

```
set DXR11_TIER11=0
```

### The whole switch list

The shim reads exactly two environment variables. There are no others.

| Setting | Default | What it does |
|---|---|---|
| `tier11` / `DXR11_TIER11` | **on** | report Tier 1.1 and rewrite RayQuery shaders |
| `nowrap` / `DXR11_NO_WRAP` | off | hand the application the real device and translate nothing |

Each can be set in `dxr11.ini` beside the DLL, or as an environment variable of
the upper-case name. The environment wins, so a stray `.ini` can never change
what the test scripts measure. Values are `1/true/on/yes` or `0/false/off/no`.

`NO_WRAP` is not a companion to the first one, it overrides it. Everything the
shim does lives on the device object it hands the application, including the
Tier 1.1 answer, so refusing to hand over that object switches the whole layer
off. With `NO_WRAP` set the application sees Tier 1.0 whatever else you set.

That leaves these states:

| DLL beside the .exe | `tier11` | `nowrap` | The application sees |
|---|---|---|---|
| no | | | Tier 1.0 |
| yes | default, or 1 | off | **Tier 1.1, shaders rewritten** |
| yes | 0 | off | Tier 1.0 |
| yes | anything | **1** | Tier 1.0, shim loaded but inert |

## 5. Read the log

Every run writes `%TEMP%\dxr11_proxy.log`. Open it first, always. It is the
only thing that tells you what the shim actually did.

```
type %TEMP%\dxr11_proxy.log
```

A healthy start looks like this:

```
[dxr-tier-11-proxy-log] attached to process, version 0.11.0
[dxr-tier-11-proxy-log] device wrapping enabled
[dxr-tier-11-proxy-log] device wrapper created (real=..., Device6=yes, Device7=yes, tier=1.0)
[dxr-tier-11-proxy-log] DXR11_TIER11=1: reporting Tier 1.1 to the application.
[dxr-tier-11-proxy-log] queue hook installed: vtable ... slot 10, self-test passed
```

The first line proves the shim loaded at all, and says which build. If the
file does not exist, the DLL is in the wrong directory and nothing else in
this guide matters.

Then, when a RayQuery shader arrives:

```
[dxr-tier-11-proxy-log] CreateComputePipelineState: shader USES RAYQUERY (SFI0 bit 20), 4812 bytes.
[dxr-tier-11-proxy-log] RayQuery compute shader lowered and ready: 4812 -> 8948 bytes, numthreads(8,8,1)
```

or, when it cannot be lowered:

```
[dxr-tier-11-proxy-log] RayQuery compute shader NOT lowered: <the reason>
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

**Turn the D3D12 debug layer on.** The shim does not switch this itself, and
it does not need to: `dxcpl.exe`, the DirectX Control Panel, forces the debug
layer on for any executable you name. It comes with the Graphics Tools
optional Windows feature (Settings, Optional features, Add a feature, Graphics
Tools). Add the target .exe to its list and enable the debug layer there.

The layer reports through `OutputDebugString`, so its messages do NOT appear in
a console or in `dxr11_proxy.log`. Read them with DebugView or a debugger
attached to the process.

`DXR11_DEBUGLAYER=1` turns it on in this project's own test harness, where the
messages are drained into stdout. That variable does nothing for the shim.

**A warning about reading anything into its silence.** The debug layer does not
police everything. It is measurably silent on hit group and geometry type
mismatches, including a case this project confirmed was producing wrong output.
An oracle that says nothing has to be shown capable of saying something before
its silence counts as a result.

## 8. Turning it off

Three levels, increasing in thoroughness:

| Goal | Do this |
|---|---|
| Report Tier 1.0 again, keep the shim loaded | `tier11 = 0` in `dxr11.ini` |
| Forward everything, translate nothing | `nowrap = 1` in `dxr11.ini` |
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
