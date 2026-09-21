# Reference shaders

Not test cases. These are compiled by hand when a question needs an answer from
DXC rather than from reasoning, and kept so the answer can be re-checked.

- `lib_array_ref.hlsl` — how does a LIBRARY index a resource array? Compile with
  `-T lib_6_3`. It showed that the global carries the array type, the element is
  reached through a constant `getelementptr`, and `createHandleForLib` takes the
  ELEMENT type. That is the form `lower.py` now emits.
