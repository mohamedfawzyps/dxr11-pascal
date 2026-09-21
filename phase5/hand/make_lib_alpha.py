#!/usr/bin/env python3
"""Hand-lower rayquery_alpha.ll (cs_6_5) into a DXR library (lib_6_5).

Pattern 3 from the brief, alpha-tested closest hit. This is the one that
matters: unlike pattern 1, it has a real `while (q.Proceed())` loop, and the
whole project rests on that loop body becoming an any-hit shader.

What makes it harder than pattern 1 is not the any-hit shader, which is small.
It is that lowering the loop DELETES six basic blocks from the raygen, and LLVM
numbers unnamed values and blocks positionally, so removing them would renumber
most of the function. So llnorm.normalize runs first and gives everything an
explicit name; after that, blocks can simply be dropped.

    python phase5/hand/make_lib_alpha.py
    phase5out\\dxilrt.exe asm phase5/hand/rayquery_alpha_lib.ll out.dxil
"""

import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from llnorm import normalize

SRC = os.path.join("phase5", "dxil", "rayquery_alpha.ll")
DST = os.path.join("phase5", "hand", "rayquery_alpha_lib.ll")


def main():
    if not os.path.isfile(SRC):
        sys.exit("run build_phase5.bat first: %s not found" % SRC)
    s = normalize(io.open(SRC, encoding="utf-8").read())

    def sub(old, new, count=1):
        nonlocal s
        assert s.count(old) == count, "expected %d of: %s" % (count, old[:70])
        s = s.replace(old, new)

    # ---------------------------------------------------------------- types
    sub('%CB = type { i32, i32, float, float, float, float, <2 x float> }',
        '%CB = type { i32, i32, float, float, float, float, <2 x float> }\n'
        '%struct.Payload = type { float, <2 x float>, i32 }\n'
        '%struct.BuiltInTriangleIntersectionAttributes = type { <2 x float> }\n'
        '\n'
        '@scene = external constant %struct.RaytracingAccelerationStructure, align 4\n'
        '@outBuf = external constant %"class.RWStructuredBuffer<Result>", align 4\n'
        '@CB = external constant %CB')

    # ------------------------------------------------------- entry function
    sub('define void @main() {', 'define void @RayGen() #1 {')

    sub('''  %v1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %v2 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %v3 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)''',
        '''  %pl = alloca %struct.Payload, align 8
  %outRaw = load %"class.RWStructuredBuffer<Result>", %"class.RWStructuredBuffer<Result>"* @outBuf, align 4
  %sceneRaw = load %struct.RaytracingAccelerationStructure, %struct.RaytracingAccelerationStructure* @scene, align 4
  %cbRaw = load %CB, %CB* @CB, align 4
  %v1 = call %dx.types.Handle @"dx.op.createHandleForLib.class.RWStructuredBuffer<Result>"(i32 160, %"class.RWStructuredBuffer<Result>" %outRaw)  ; CreateHandleForLib(Resource)
  %v2 = call %dx.types.Handle @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure(i32 160, %struct.RaytracingAccelerationStructure %sceneRaw)  ; CreateHandleForLib(Resource)
  %v3 = call %dx.types.Handle @dx.op.createHandleForLib.CB(i32 160, %CB %cbRaw)  ; CreateHandleForLib(Resource)''')

    sub('%v4 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)',
        '%v4 = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 0)  ; DispatchRaysIndex(col)')
    sub('%v5 = call i32 @dx.op.threadId.i32(i32 93, i32 1)  ; ThreadId(component)',
        '%v5 = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 1)  ; DispatchRaysIndex(col)')

    # ------------------------------------------------- the loop disappears
    # Everything from the allocate to the block after the loop is replaced by
    # a payload init and one TraceRay. That removes bb35 (preheader), bb36
    # (loop header), bb39, bb44 (latch), bb46 (the commit) and bb47.
    #
    # The driver now runs the traversal that the Proceed loop was running by
    # hand, and calls the any-hit shader below for each non-opaque candidate.
    start = s.index('  %v33 = call i32 @dx.op.allocateRayQuery')
    end = s.index('bb48:')
    assert start < end
    s = s[:start] + '''  %plT = getelementptr inbounds %struct.Payload, %struct.Payload* %pl, i32 0, i32 0
  store float 0.000000e+00, float* %plT, align 8
  %plB = getelementptr inbounds %struct.Payload, %struct.Payload* %pl, i32 0, i32 1
  store <2 x float> zeroinitializer, <2 x float>* %plB, align 4
  %plH = getelementptr inbounds %struct.Payload, %struct.Payload* %pl, i32 0, i32 2
  store i32 0, i32* %plH, align 4
  call void @dx.op.traceRay.struct.Payload(i32 157, %dx.types.Handle %v2, i32 0, i32 255, i32 0, i32 0, i32 0, float %v27, float %v28, float %v29, float %v31, float 0.000000e+00, float 0.000000e+00, float -1.000000e+00, float %v32, %struct.Payload* nonnull %pl)  ; TraceRay(...)
  br label %bb48

''' + s[end:]

    # bb48 lost a predecessor along with the loop.
    sub('bb48:                                      ; preds = %bb47, %bb12',
        'bb48:                                      ; preds = %bb12')

    # The committed accessors become payload reads, exactly as in pattern 1.
    sub('  %v49 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 184, i32 %v33)  ; RayQuery_CommittedStatus(rayQueryHandle)',
        '  %v49 = load i32, i32* %plH, align 4')
    sub('''  %v52 = call float @dx.op.rayQuery_StateScalar.f32(i32 200, i32 %v33)  ; RayQuery_CommittedRayT(rayQueryHandle)
  %v53 = call float @dx.op.rayQuery_StateVector.f32(i32 194, i32 %v33, i8 0)  ; RayQuery_CommittedTriangleBarycentrics(rayQueryHandle,component)
  %v54 = call float @dx.op.rayQuery_StateVector.f32(i32 194, i32 %v33, i8 1)  ; RayQuery_CommittedTriangleBarycentrics(rayQueryHandle,component)''',
        '''  %v52 = load float, float* %plT, align 8
  %plBv = load <2 x float>, <2 x float>* %plB, align 4
  %v53 = extractelement <2 x float> %plBv, i32 0
  %v54 = extractelement <2 x float> %plBv, i32 1''')

    # ------------------------------------------- the generated shader set
    # AnyHit IS the Proceed loop body. The arithmetic below is the same three
    # instructions the loop ran, re-rooted onto the attribute parameter:
    #
    #   loop body                          any-hit
    #   CandidateTriangleBarycentrics(.x)  extractelement(load attr, 0)
    #   fmul 4.0, Frc, fcmp olt 0.5        identical
    #   CommitNonOpaqueTriangleHit         fall off the end
    #   fall through to the latch          IgnoreHit
    #
    # Note the polarity inverts: committing is the special path in RayQuery,
    # ignoring is the special path in any-hit.
    #
    # The loop's `CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE` test is
    # DROPPED, because an any-hit shader is only ever invoked for exactly that
    # case. A CANDIDATE_PROCEDURAL_PRIMITIVE arm would have to become an
    # intersection shader instead, which pattern 3 does not exercise.
    sub('''; Function Attrs: nounwind readnone
declare i32 @dx.op.threadId.i32(i32, i32) #0''',
        '''define void @AnyHit(%struct.Payload* noalias nocapture %p, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attr) #1 {
  %a = getelementptr inbounds %struct.BuiltInTriangleIntersectionAttributes, %struct.BuiltInTriangleIntersectionAttributes* %attr, i32 0, i32 0
  %b = load <2 x float>, <2 x float>* %a, align 4
  %bx = extractelement <2 x float> %b, i32 0
  %m = fmul fast float %bx, 4.000000e+00
  %f = call float @dx.op.unary.f32(i32 22, float %m)  ; Frc(value)
  %c = fcmp fast olt float %f, 5.000000e-01
  br i1 %c, label %accept, label %reject

accept:                                           ; preds = %0
  ret void

reject:                                           ; preds = %0
  call void @dx.op.ignoreHit(i32 155)  ; IgnoreHit()
  unreachable
}

define void @ClosestHit(%struct.Payload* noalias nocapture %p, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attr) #1 {
  %1 = call float @dx.op.rayTCurrent.f32(i32 154)  ; RayTCurrent()
  %2 = getelementptr inbounds %struct.BuiltInTriangleIntersectionAttributes, %struct.BuiltInTriangleIntersectionAttributes* %attr, i32 0, i32 0
  %3 = load <2 x float>, <2 x float>* %2, align 4
  %4 = getelementptr inbounds %struct.Payload, %struct.Payload* %p, i32 0, i32 0
  store float %1, float* %4, align 4
  %5 = getelementptr inbounds %struct.Payload, %struct.Payload* %p, i32 0, i32 1
  store <2 x float> %3, <2 x float>* %5, align 4
  %6 = getelementptr inbounds %struct.Payload, %struct.Payload* %p, i32 0, i32 2
  store i32 1, i32* %6, align 4
  ret void
}

define void @Miss(%struct.Payload* noalias nocapture %p) #1 {
  %1 = getelementptr inbounds %struct.Payload, %struct.Payload* %p, i32 0, i32 2
  store i32 0, i32* %1, align 4
  ret void
}

; Function Attrs: nounwind readnone
declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #0

; Function Attrs: nounwind
declare void @dx.op.traceRay.struct.Payload(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, float, float, float, float, float, %struct.Payload*) #1

; Function Attrs: noreturn nounwind
declare void @dx.op.ignoreHit(i32) #3

; Function Attrs: nounwind readonly
declare float @dx.op.rayTCurrent.f32(i32) #2

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure(i32, %struct.RaytracingAccelerationStructure) #2

; Function Attrs: nounwind readonly
declare %dx.types.Handle @"dx.op.createHandleForLib.class.RWStructuredBuffer<Result>"(i32, %"class.RWStructuredBuffer<Result>") #2

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandleForLib.CB(i32, %CB) #2''')

    for decl in [
        '\n; Function Attrs: nounwind\ndeclare i32 @dx.op.allocateRayQuery(i32, i32) #1\n',
        '\n; Function Attrs: nounwind\ndeclare void @dx.op.rayQuery_TraceRayInline(i32, i32, %dx.types.Handle, i32, i32, float, float, float, float, float, float, float, float) #1\n',
        '\n; Function Attrs: nounwind readonly\ndeclare float @dx.op.rayQuery_StateVector.f32(i32, i32, i8) #2\n',
        '\n; Function Attrs: nounwind readonly\ndeclare float @dx.op.rayQuery_StateScalar.f32(i32, i32) #2\n',
        '\n; Function Attrs: nounwind readonly\ndeclare i32 @dx.op.rayQuery_StateScalar.i32(i32, i32) #2\n',
        '\n; Function Attrs: nounwind\ndeclare void @dx.op.rayQuery_CommitNonOpaqueTriangleHit(i32, i32) #1\n',
        '\n; Function Attrs: nounwind\ndeclare i1 @dx.op.rayQuery_Proceed.i1(i32, i32) #1\n',
        '\n; Function Attrs: nounwind readonly\ndeclare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #2\n',
    ]:
        sub(decl, '\n')

    # IgnoreHit is noreturn, and the input has no attribute group for that.
    sub('attributes #2 = { nounwind readonly }',
        'attributes #2 = { nounwind readonly }\nattributes #3 = { noreturn nounwind }')

    # ------------------------------------------------------------ metadata
    sub('!dx.entryPoints = !{!13}',
        '!dx.typeAnnotations = !{!13}\n!dx.entryPoints = !{!21, !23, !26, !28, !30}')
    sub('!3 = !{!"cs", i32 6, i32 5}', '!3 = !{!"lib", i32 6, i32 5}')

    sub('!6 = !{i32 0, %struct.RaytracingAccelerationStructure* undef, !"", i32 0, i32 0, i32 1, i32 16, i32 0, !7}',
        '!6 = !{i32 0, %struct.RaytracingAccelerationStructure* @scene, !"scene", i32 0, i32 0, i32 1, i32 16, i32 0, !7}')
    sub('!9 = !{i32 0, %"class.RWStructuredBuffer<Result>"* undef, !"", i32 0, i32 0, i32 1, i32 12, i1 false, i1 false, i1 false, !10}',
        '!9 = !{i32 0, %"class.RWStructuredBuffer<Result>"* @outBuf, !"outBuf", i32 0, i32 0, i32 1, i32 12, i1 false, i1 false, i1 false, !10}')
    sub('!12 = !{i32 0, %CB* undef, !"", i32 0, i32 0, i32 1, i32 32, null}',
        '!12 = !{i32 0, %CB* @CB, !"CB", i32 0, i32 0, i32 1, i32 32, null}')

    # Shader kinds: 7 raygeneration, 9 anyhit, 10 closesthit, 11 miss.
    # Tag 6 is payload bytes (16), tag 7 attribute bytes (8), tag 5 auto
    # binding space. Confirmed against the DXC-built library in the recon.
    sub('''!13 = !{void ()* @main, !"main", null, !4, !14}
!14 = !{i32 0, i64 33554448, i32 4, !15}
!15 = !{i32 8, i32 8, i32 1}''',
        '''!13 = !{i32 1, void ()* @RayGen, !14, void (%struct.Payload*, %struct.BuiltInTriangleIntersectionAttributes*)* @AnyHit, !17, void (%struct.Payload*, %struct.BuiltInTriangleIntersectionAttributes*)* @ClosestHit, !17, void (%struct.Payload*)* @Miss, !20}
!14 = !{!15}
!15 = !{i32 1, !16, !16}
!16 = !{}
!17 = !{!15, !18, !19}
!18 = !{i32 2, !16, !16}
!19 = !{i32 0, !16, !16}
!20 = !{!15, !18}
!21 = !{null, !"", null, !4, !22}
!22 = !{i32 0, i64 16}
!23 = !{void (%struct.Payload*, %struct.BuiltInTriangleIntersectionAttributes*)* @AnyHit, !"AnyHit", null, null, !24}
!24 = !{i32 8, i32 9, i32 6, i32 16, i32 7, i32 8, i32 5, !25}
!25 = !{i32 0}
!26 = !{void (%struct.Payload*, %struct.BuiltInTriangleIntersectionAttributes*)* @ClosestHit, !"ClosestHit", null, null, !27}
!27 = !{i32 8, i32 10, i32 6, i32 16, i32 7, i32 8, i32 5, !25}
!28 = !{void (%struct.Payload*)* @Miss, !"Miss", null, null, !29}
!29 = !{i32 8, i32 11, i32 6, i32 16, i32 5, !25}
!30 = !{void ()* @RayGen, !"RayGen", null, null, !31}
!31 = !{i32 8, i32 7, i32 5, !25}''')

    io.open(DST, "w", encoding="utf-8", newline="\n").write(s)
    print("wrote", DST)


if __name__ == "__main__":
    main()
