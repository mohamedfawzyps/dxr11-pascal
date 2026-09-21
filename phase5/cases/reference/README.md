# Reference shaders

Not test cases. These are compiled by hand when a question needs an answer from
DXC rather than from reasoning, and kept so the answer can be re-checked.

- `lib_array_ref.hlsl` — how does a LIBRARY index a resource array? Compile with
  `-T lib_6_3`. It showed that the global carries the array type, the element is
  reached through a constant `getelementptr`, and `createHandleForLib` takes the
  ELEMENT type. That is the form `lower.py` now emits.
- `lib_ids_ref.hlsl` - which dx.op opcodes are `InstanceIndex`,
  `PrimitiveIndex` and `GeometryIndex` in a DXR 1.0 closest-hit? Compile
  with `-T lib_6_5`. Answer: 142, 161 and 213. Only the first two are
  usable on Tier 1.0; `GeometryIndex` sets shader flag 0x2000000 and the
  GTX 1070 refuses the state object.

- `lib_accessors_ref.hlsl` - are the DXR 1.0 hit-shader intrinsics that would
  serve Unreal's missing RayQuery accessors actually Tier 1.0? Compile with
  `-T lib_6_5`. PrimitiveIndex, InstanceIndex, InstanceID, HitKind,
  RayTCurrent, ObjectRayOrigin, ObjectRayDirection and WorldToObject4x3 all
  give SFI0=0x0, so all are safe. GeometryIndex gives 0x100000 and
  InstanceContributionToHitGroupIndex does not exist in HLSL at all.

- `lib_null_ref.hlsl` - what does a hit group that must NEVER commit compile
  to, and does either half of it pull in a shader feature flag Tier 1.0
  refuses? Compile with `-T lib_6_5`. Answers: an always-ignoring any-hit is
  `call void @dx.op.ignoreHit(i32 155)` then `unreachable`, marked
  `noreturn nounwind`; a no-op intersection shader is a bare `ret void`. Both
  give SFI0=0x0, so neither costs anything on Pascal. That is the form
  `lower.py` and `rq_lower.cpp` now emit as `AnyHitNull` and `IsectNull`, which
  are what let the shader table carry a record of the right TYPE at an index
  reached by geometry the lowered shader does not serve.

- `lib_dynarray_ref.hlsl` - what does a resource array indexed by a NON-constant
  compile to in a library, and what does `NonUniformResourceIndex` add? Compile
  with `-T lib_6_5`. Answers: the constant case's folded getelementptr
  EXPRESSION becomes a real getelementptr INSTRUCTION on the same global,
  followed by the same load and `createHandleForLib`; and
  `NonUniformResourceIndex` attaches `!dx.nonuniform !N` to that instruction,
  where the node is `!{i32 1}`. Nothing else changes, no new opcode and no
  annotateHandle. That is the form `lower.py` and `rq_lower.cpp` now emit.
