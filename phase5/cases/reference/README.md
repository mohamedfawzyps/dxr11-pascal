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
