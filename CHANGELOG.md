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
