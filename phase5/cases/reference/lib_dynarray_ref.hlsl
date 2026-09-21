// Reference only: what does DXC emit for a resource array indexed by a value
// that is NOT a compile-time constant, in a LIBRARY? Compile with -T lib_6_5.
//
// The constant case is lib_array_ref.hlsl: the element is reached by a
// constant getelementptr folded into the createHandleForLib argument. A
// dynamic index cannot be a constant expression, so something has to change,
// and the question is what: a real getelementptr instruction, a different
// opcode, or an annotateHandle in between.
//
// Both forms are here because they are different in HLSL and may be different
// in DXIL. NonUniformResourceIndex is what an application writes when the
// index can vary across a wave, and it sets a flag the shim has so far
// refused outright.
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBufs[4] : register(u0);
cbuffer CB : register(b0) { uint pick; };

[shader("raygeneration")]
void RayGen() {
    Result r = (Result)0; r.hit = 1;
    outBufs[pick][DispatchRaysIndex().x] = r;
}

[shader("raygeneration")]
void RayGenNonUniform() {
    Result r = (Result)0; r.hit = 2;
    outBufs[NonUniformResourceIndex(pick)][DispatchRaysIndex().x] = r;
}
