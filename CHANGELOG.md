# Changelog

## Versioning

[Semantic Versioning](https://semver.org/). The major version is the honest
part:

- **0.x** while the tier flip is opt-in. The refusal list has only ever been
  exercised against this project's own shaders, so the boundary between "runs"
  and "refused" is measured but not yet trusted.
- **1.0.0** when real software has run through it and that boundary holds.
  That is a statement about exposure, not about features.

MINOR adds lowering coverage or a feature. PATCH is fixes only. The version is
compiled into the DLL and logged on attach, so a log file identifies its own
build.

---

## 0.13.0 (2026-09-22)

### Renamed everything that read as DirectX 11

`dxr11` parses as "DirectX 11" on sight, which is the opposite of what this
does. The repository was renamed to `pascal-dxr-tier-1.1` for that reason and
the rest had not caught up.

| Was | Is |
|---|---|
| `dxr11.ini` | `dxr-tier-11.ini` |
| `dxr11.example.ini` | `dxr-tier-11.example.ini` |
| `DXR11_TIER11` | `DXR_TIER11` |
| `DXR11_NO_WRAP` | `DXR_TIER11_NOWRAP` |
| `DXR11_DEBUGLAYER` | `DXR_TIER11_DEBUGLAYER` (test harness only) |
| `%TEMP%\dxr11_proxy.log` | `%TEMP%\dxr-tier-11-proxy.log` |

**A clean break, with no compatibility shims for the old names.** None of these
were ever in a published release, so nothing exists to be compatible with.
Accepting both would be carrying a name this project renamed on purpose.

Internal C++ identifiers are deliberately untouched. `Dxr11Device`,
`Dxr11CommandList` and the rest are not user-visible, and renaming them would
be a very large diff for no reader's benefit.

Earlier entries in this file were also left alone. Those releases really did
use the old names, and rewriting the history would make it describe things that
never happened.

### Verified

Three behaviours re-checked by name after the sweep, since a rename that
silently stops reading a setting looks exactly like a working default:
`dxr-tier-11-proxy.log` is written, `DXR_TIER11=0` turns the tier off, and
`tier11 = 0` in `dxr-tier-11.ini` does too.

Full regression unchanged: 14 render cases, 4 gates, 12 rewriter cases
byte-identical on both paths, 14 analysis checks, the probe at
14450 + 2312 + 48774 = 65536, and `D3D12RaytracingHelloWorld` 0 of 14400 pixels
different with the wrapper running.

---

## 0.12.0 (2026-09-22)

### Without DXC the shim now stands aside completely

0.11.0 stopped claiming Tier 1.1 when `dxcompiler.dll` and `dxil.dll` are
missing. This goes further: it stops wrapping the device at all.

Everything the wrapper still handles is a Tier 1.1 feature, `AddToStateObject`,
indirect `DispatchRays`, and the command list and acceleration structure
machinery those need. Reporting Tier 1.0 means an application that reads the
tier it was given never calls any of them, so the wrapper would sit in the path
of every device and command list call waiting for work that cannot arrive.

That is risk with no benefit, and the risk is not theoretical: the wrapper
hooks a queue vtable and wraps every command list.

    standing aside entirely: dxcompiler.dll is not next to the shim ... The
    application gets the real device untouched, which is what it would have had
    with no shim installed at all.

The log survives, because the lines that say what happened come from the proxy
layer rather than from the wrapper.

**The cost:** DXC is now loaded at device creation rather than at the first
RayQuery shader. The lazy loading noted in earlier work is gone when DXC is
present. Two `LoadLibrary` calls at startup, in exchange for doing nothing at
all when it is absent.

### A test had quietly stopped testing anything

`run_proxy_test.ps1` compares a Microsoft sample with and without the shim and
asserts the frames are pixel identical. Its whole purpose is to show the
WRAPPER is transparent.

The sample folders contain no DXC, so after the change above the shim stood
aside there and the comparison became the unmodified path against itself. It
would have passed forever while proving nothing.

The script now copies DXC beside the proxy, and warns loudly if it cannot. Both
samples are verified again with the wrapper actually running.

That is the fourth time in this project a test was found measuring nothing, and
the first where a correct change to the product caused it.

### Renamed

The log prefix is `[dxr-tier-11-proxy-log]`, since `dxr11` reads as DirectX 11.

---

## 0.11.0 (2026-09-22)

### Tier 1.1 is no longer claimed without DXC

Claiming Tier 1.1 is a promise to translate RayQuery, and that promise cannot
be kept without `dxcompiler.dll` and `dxil.dll`. The shim now checks, and
reports the real tier when they are absent.

This was the exact failure the project refuses to ship, and 0.10.0 had
introduced it by making the tier claim the default: copy the DLL on its own,
forget the other two files, and an application is told it may emit RayQuery,
does so, and the driver rejects a shader nobody could rewrite. The application
crashes and the cause is only in a log, after the fact.

Verified both directions in a directory containing nothing but the executable
and `d3d12.dll`:

    [dxr-tier-11-proxy-log] NOT reporting Tier 1.1: dxcompiler.dll is not next to the
    shim ... Tier 1.0 is reported instead, which is honest

then with the two DXC files added, the claim returns. NOT in the automated
suite: it needs a directory without DXC and the test harness compiles its own
shaders with DXC. Verified by hand, and said plainly here rather than implied.

The cost is that DXC now loads at the first feature query rather than at the
first shader. That is the point: better to find out early.

### Renamed

`tools/dxr11-setup.*` is now `tools/dxr-tier-11-setup.*`. "dxr11" reads as
DirectX 11 at a glance, which is the opposite of what this does.

---

## 0.10.0 (2026-09-22)

Makes the thing usable without a terminal. No change to any translation, and
the whole regression is unchanged.

### Tier 1.1 is now ON by default

The opt-in already happened. A proxy DLL only sits beside an executable
because somebody deliberately put it there, and that is the consent. Requiring
a second one, through an environment variable that a launcher never passes on,
mostly produced reports that the shim does nothing.

Turning it off is still one step: delete the DLL, or `tier11 = 0`.

The gate this replaces was worth having during development, where the risk was
a test run silently flipping. It is the wrong default for a release.

### Settings live in a file now

`dxr11.ini`, beside the DLL. A game started from Steam or the Epic launcher
never sees an environment variable you set in a console, so a file is the only
mechanism that works regardless of how something is launched. See
`dxr11.example.ini`.

Environment variables of the same name still work and **take priority**, so a
stray `.ini` can never change what the test scripts measure.

### The log says which DXC produced a shader

    [dxr-tier-11-proxy-log] rewriter using dxcompiler 1.10.2605.37, dxil 1.10.2605.37

The first question about any signing or validation failure is which compiler
was involved, and the log could not answer it. Read from the file version
resource rather than `IDxcVersionInfo`, which reports only major and minor,
because a bug report needs the build number to name a release.

### Verified

The default flip is the part that could have broken something, so the case
that matters is a DXR 1.0 application now being told Tier 1.1 with no
configuration at all. `D3D12RaytracingHelloWorld` still renders 0 of 14400
pixels different at unchanged fps, with the log confirming Tier 1.1 was
reported.

Everything else unchanged: 14 render cases, 12 rewriter cases byte-identical
on both paths, 14 analysis and lowering checks, the probe at
14450 + 2312 + 48774 = 65536.

The gate tests were rewritten rather than deleted. They used to assert the
flip could not happen by accident. They now assert the off switch works, by
environment variable and by file, which is what a bug report will be asked to
try first.

---

## 0.9.0 (2026-09-21)

First tagged release. A `RayQuery` compute shader, unmodified, runs on a GTX
1070 and produces bit-exact output against WARP.

This is a snapshot taken deliberately **before** pointing the shim at a real
engine, so that whatever the engine turns up can be measured against a fixed
point.

### Tier 1.1 features

- **Ray flags** (`SKIP_TRIANGLES`, `SKIP_PROCEDURAL_PRIMITIVES`) need no shim.
  Measured against WARP: the Tier 1.0 driver already honours them.
- **`AddToStateObject`** emulated by stripping `ALLOW_STATE_OBJECT_ADDITIONS`,
  caching the subobjects and rebuilding a whole new state object.
- **Indirect `DispatchRays`** emulated by splitting the command list at the
  dispatch, reading back the argument buffer and issuing real `DispatchRays`
  calls. Binding state survives the split.
- **Inline ray tracing** by rewriting the DXIL into a DXR 1.0 library with
  generated any-hit, intersection, closest-hit and miss shaders.

### Inline ray tracing coverage

- Opaque closest hit, alpha-tested closest hit, shadow/visibility, procedural
  primitives, and a query committing both triangle and procedural hits from one
  `Proceed` loop
- `Abort()`, carried as a payload flag
- 21 of the 25 `RayQuery` accessors Unreal Engine 5.7 uses. The other four have
  no DXR 1.0 equivalent
- Resource arrays at constant, dynamic and non-uniform indices
- Acceleration structure interception, so the shader table is sized and typed
  from the application's own geometry layout rather than assumed

### Verification

All on a GTX 1070, with WARP as the oracle on every run rather than a stored
baseline:

- 14 end-to-end render cases, all bit-exact, plus 2 refusal gates
- 12 rewriter cases, each byte-identical between the Python reference and the
  C++ port, on both the `.ll` and DXIL container paths
- 9 refusals provoked by tests
- Two Microsoft DXR 1.0 samples unchanged through the proxy, one of them
  pixel-identical to its no-proxy baseline

### Known issues

Recorded because they are known, not because they are acceptable:

- **A `RayQuery` shader with no early-out bounds check fails to lower.** The
  entry block becomes a phi predecessor and normalization leaves a dangling
  label reference. It fails at the assembler with a message that names nothing
  relevant. Every shader in the test suite happens to open with a bounds check,
  which is why this was not caught earlier.
- **`CreatePipelineState`** (the pipeline stream form) detects `RayQuery` but
  forwards it instead of lowering it. Only `CreateComputePipelineState` gets
  the substitution.
- **`createHandleFromHeap`** is documented as refused and is not. A bindless
  SM 6.6 shader is accepted and fails later for the reason above.
- **More than one `RayQuery` object per entry point is refused**, including
  plainly sequential ones. Primary plus shadow in a single compute shader is a
  common real shape.
- **No real application has run through this yet.** Unreal Engine coverage is a
  source survey, not an execution test, and the survey found Epic using all
  four of the accessors that cannot be lowered.

### Notes

The tier flip stays opt-in behind `DXR11_TIER11=1`. Reporting Tier 1.1
entitles an application to emit `RayQuery`, and a shim that claims 1.1 and then
fails is worse than one that claims 1.0.
