# Phase 4: AddToStateObject emulation

**Status: WORKING on the GTX 1070 (2026-09-21).** The Phase 4 probe now reports
the same result on Tier 1.0 hardware as it does on WARP.

Source: `proxy/state_object_cache.{h,cpp}` plus the `CreateStateObject` and
`AddToStateObject` overrides in `proxy/d3d12_device.cpp`.

## The problem

On Tier 1.0 the driver refuses the state object before growth is ever attempted:

    ID3D12Device::CreateStateObject: Invalid D3D12_STATE_OBJECT_FLAGS: 0x4

0x4 is `ALLOW_STATE_OBJECT_ADDITIONS`. So there are two interception points, not
one, and the first is the one that actually blocks the app.

## The approach

`CreateStateObject` strips the flag, forwards the stripped desc, and keeps a
deep copy of everything the app asked for. `AddToStateObject` merges that copy
with the addition and rebuilds a complete state object.

Rebuilding rather than growing is the point. The driver has no incremental path
on Tier 1.0, but it will happily build a complete object that contains
everything the app has asked for so far. The cost is a full compile on every
addition, which this project explicitly accepts; the brief says 1 fps is a fine
outcome and correctness is what matters.

On Tier 1.1 none of this runs. The wrapper reads `RaytracingTier` once at
construction and forwards `CreateStateObject` and `AddToStateObject` untouched
when the device is already capable, so the shim stays transparent on hardware
that does not need it. Verified: on WARP the log shows
`AddToStateObject forwarded (tier 1.1)` and no caching at all.

## The hard part: copying a D3D12_STATE_OBJECT_DESC

A `D3D12_STATE_OBJECT_DESC` is a flat array of subobjects, but each one points
at a type-specific desc that in turn points at wide strings, shader bytecode,
export arrays and COM objects. None of that has to stay alive after
`CreateStateObject` returns, and we rebuild much later, so all of it is copied:
bytecode into owned buffers, strings interned, root signatures and collections
AddRef'd.

Every container holding those copies is a `std::deque`, not a `std::vector`,
because the subobject array points into them and deque does not move existing
elements as it grows.

**The awkward case is `SUBOBJECT_TO_EXPORTS_ASSOCIATION`**, which holds a
pointer *into the same subobject array*. A naive copy leaves it aimed at the
caller's stack. Instead the copy records which index it referred to and the
pointer is resolved in `Desc()`, once the array has stopped moving. It must
point into the *final* array that D3D12 sees, not the working one, which is a
second trap: merging removes entries, so the two arrays do not share indices.

## Merging

The Phase 4 probe established that an addition must repeat the shader config,
the pipeline config and the config flag, because those are not inherited by new
exports. A naive concatenation therefore produces duplicates of subobject types
that may appear only once.

The merge drops a repeated singleton when the base already has one, keeping the
base's, and logs each drop:

    [dxr11-proxy]   merge: dropping duplicate STATE_OBJECT_CONFIG from addition
    [dxr11-proxy]   merge: dropping duplicate RAYTRACING_SHADER_CONFIG from addition
    [dxr11-proxy]   merge: dropping duplicate RAYTRACING_PIPELINE_CONFIG from addition

Dropped entries leave a placeholder so index arithmetic still works, and an
association that pointed at a dropped singleton is retargeted at the surviving
subobject of the same type.

**Known simplification:** keeping the base's config means an addition that tries
to *raise* one, a larger `MaxPayloadSizeInBytes` for instance, would be ignored.
The drop is logged rather than silent, so this would be visible rather than
mysterious. No test app does it yet, and nothing has been built to handle it.

## Lifetime

The cache is attached to the state object itself with
`SetPrivateDataInterface`, under a private GUID. The object owns a reference to
the holder, so the copy is freed exactly when the object it describes is. A side
table keyed on the raw pointer was rejected: it would leak, and worse, a freed
object's address can be reused and pick up a stale entry. A grown object gets
its own entry, so it can be grown again.

## Unsupported subobjects fail loudly

`Append` refuses a subobject type it does not understand rather than dropping
it, because a dropped subobject builds a state object that differs from the
request, which is the worst possible outcome here. Supported:
`STATE_OBJECT_CONFIG`, `GLOBAL_ROOT_SIGNATURE`, `LOCAL_ROOT_SIGNATURE`,
`NODE_MASK`, `DXIL_LIBRARY`, `EXISTING_COLLECTION`,
`SUBOBJECT_TO_EXPORTS_ASSOCIATION`, `DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION`,
`RAYTRACING_SHADER_CONFIG`, `RAYTRACING_PIPELINE_CONFIG`,
`RAYTRACING_PIPELINE_CONFIG1`, `HIT_GROUP`. Anything else, work graphs for
instance, logs a named refusal and the object is created without caching, so
additions to it fail cleanly instead of producing something wrong.

## Verification

Phase 4 probe on the GTX 1070, through the proxy. Before, versus after:

| | before | after |
|---|---|---|
| base SO with the flag | FAILED E_INVALIDARG | **OK** |
| AddToStateObject | not reached | **OK** |
| identifiers after growth | - | RayGen, HitGroup, Miss2 all resolve |
| dispatch with added Miss2 | - | 14450 tri + 2312 proc + 48774 Miss2 |

That is 65536 rays exactly, and **identical to the WARP ground truth**, which is
the claim that matters: the emulated path produces the same image as a real Tier
1.1 implementation.

The proxy log shows the mechanism end to end:

    CreateStateObject: stripped ALLOW_STATE_OBJECT_ADDITIONS, 7 subobjects cached, hr=0x00000000
      merge: dropping duplicate STATE_OBJECT_CONFIG from addition
      merge: dropping duplicate RAYTRACING_SHADER_CONFIG from addition
      merge: dropping duplicate RAYTRACING_PIPELINE_CONFIG from addition
    AddToStateObject EMULATED: base 7 + addition 4 -> 8 subobjects, rebuilt hr=0x00000000

With `-debug`, the D3D12 debug layer says nothing at all about the rebuilt
object. The only message in the run is the expected `CreateCommandSignature`
refusal, which is the next feature.

Regression, all unchanged: raytest ALL MATCH, HelloWorld 0 of 14400 pixels
differ, SimpleLighting unchanged fps. Neither sample enters the caching path,
since neither asks for additions, so the fast path is intact.

## Still to do in Phase 4

Indirect DispatchRays, then flip `CheckFeatureSupport` to report Tier 1.1 last.
