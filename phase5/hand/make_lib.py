#!/usr/bin/env python3
"""Hand-lower rayquery_opaque.ll (cs_6_5) into a DXR library (lib_6_5).

This is pattern 1 from the brief, opaque closest hit, done BY HAND before any
rewriter exists. Phase 2 made the same move at the HLSL level: find out what
breaks by hand now, not inside a compiler later. The validator's complaints are
the specification the automated pass will have to satisfy.

The transform edits the input module in place rather than building a new one,
which is what the Phase 5 recon concluded. The application's own code never
moves between modules, so its resource bindings cannot be got wrong in transit.

Every value this script introduces is NAMED, never numbered, because LLVM
numbers unnamed values sequentially and inserting a numbered one would shift
every value after it.

    python phase5/hand/make_lib.py
    phase5out\\dxilrt.exe asm phase5/hand/rayquery_opaque_lib.ll out.dxil
"""

import io
import os
import sys

SRC = os.path.join("phase5", "dxil", "rayquery_opaque.ll")
DST = os.path.join("phase5", "hand", "rayquery_opaque_lib.ll")


def main():
    if not os.path.isfile(SRC):
        sys.exit("run build_phase5.bat first: %s not found" % SRC)
    s = io.open(SRC, encoding="utf-8").read()

    def sub(old, new, count=1):
        nonlocal s
        assert s.count(old) == count, "expected %d of: %s" % (count, old[:70])
        s = s.replace(old, new)

    # ---------------------------------------------------------------- types
    # A library needs the payload and the built-in triangle attributes, and it
    # needs the resources as real globals: lib resource handles come from
    # createHandleForLib on a loaded global, not from a binding index.
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

    # Resource handles. The three numbered results stay numbered so nothing
    # downstream shifts; the loads that feed them are named.
    sub('''  %1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %2 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)
  %3 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)  ; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)''',
        '''  %pl = alloca %struct.Payload, align 8
  %outRaw = load %"class.RWStructuredBuffer<Result>", %"class.RWStructuredBuffer<Result>"* @outBuf, align 4
  %sceneRaw = load %struct.RaytracingAccelerationStructure, %struct.RaytracingAccelerationStructure* @scene, align 4
  %cbRaw = load %CB, %CB* @CB, align 4
  %1 = call %dx.types.Handle @"dx.op.createHandleForLib.class.RWStructuredBuffer<Result>"(i32 160, %"class.RWStructuredBuffer<Result>" %outRaw)  ; CreateHandleForLib(Resource)
  %2 = call %dx.types.Handle @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure(i32 160, %struct.RaytracingAccelerationStructure %sceneRaw)  ; CreateHandleForLib(Resource)
  %3 = call %dx.types.Handle @dx.op.createHandleForLib.CB(i32 160, %CB %cbRaw)  ; CreateHandleForLib(Resource)''')

    # DispatchRays has no thread group, so the ray index replaces the thread id.
    sub('%4 = call i32 @dx.op.threadId.i32(i32 93, i32 0)  ; ThreadId(component)',
        '%4 = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 0)  ; DispatchRaysIndex(col)')
    sub('%5 = call i32 @dx.op.threadId.i32(i32 93, i32 1)  ; ThreadId(component)',
        '%5 = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 1)  ; DispatchRaysIndex(col)')

    # ------------------------------------------------- the query itself
    # RAY_FLAG_FORCE_OPAQUE means Proceed never yields a candidate, so there is
    # no any-hit shader and no loop. The query collapses to one TraceRay whose
    # results arrive in the payload.
    #
    # The CFG below is left exactly as it was, phi nodes and all. That works
    # because CommittedStatus is compared against COMMITTED_TRIANGLE_HIT (1) and
    # the generated ClosestHit writes hit=1 while Miss writes hit=0, so reading
    # the payload's hit field is the same test.
    sub('''  %33 = call i32 @dx.op.allocateRayQuery(i32 178, i32 1)  ; AllocateRayQuery(constRayFlags)
  call void @dx.op.rayQuery_TraceRayInline(i32 179, i32 %33, %dx.types.Handle %2, i32 1, i32 255, float %27, float %28, float %29, float %31, float 0.000000e+00, float 0.000000e+00, float -1.000000e+00, float %32)  ; RayQuery_TraceRayInline(rayQueryHandle,accelerationStructure,rayFlags,instanceInclusionMask,origin_X,origin_Y,origin_Z,tMin,direction_X,direction_Y,direction_Z,tMax)
  %34 = call i1 @dx.op.rayQuery_Proceed.i1(i32 180, i32 %33)  ; RayQuery_Proceed(rayQueryHandle)
  %35 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 184, i32 %33)  ; RayQuery_CommittedStatus(rayQueryHandle)''',
        '''  %33 = getelementptr inbounds %struct.Payload, %struct.Payload* %pl, i32 0, i32 0
  store float 0.000000e+00, float* %33, align 8
  %34 = getelementptr inbounds %struct.Payload, %struct.Payload* %pl, i32 0, i32 1
  store <2 x float> zeroinitializer, <2 x float>* %34, align 4
  %plH = getelementptr inbounds %struct.Payload, %struct.Payload* %pl, i32 0, i32 2
  store i32 0, i32* %plH, align 4
  call void @dx.op.traceRay.struct.Payload(i32 157, %dx.types.Handle %2, i32 1, i32 255, i32 0, i32 0, i32 0, float %27, float %28, float %29, float %31, float 0.000000e+00, float 0.000000e+00, float -1.000000e+00, float %32, %struct.Payload* nonnull %pl)  ; TraceRay(...)
  %35 = load i32, i32* %plH, align 4''')

    # The committed accessors become payload reads.
    sub('''  %38 = call float @dx.op.rayQuery_StateScalar.f32(i32 200, i32 %33)  ; RayQuery_CommittedRayT(rayQueryHandle)
  %39 = call float @dx.op.rayQuery_StateVector.f32(i32 194, i32 %33, i8 0)  ; RayQuery_CommittedTriangleBarycentrics(rayQueryHandle,component)
  %40 = call float @dx.op.rayQuery_StateVector.f32(i32 194, i32 %33, i8 1)  ; RayQuery_CommittedTriangleBarycentrics(rayQueryHandle,component)''',
        '''  %38 = load float, float* %33, align 8
  %plBv = load <2 x float>, <2 x float>* %34, align 4
  %39 = extractelement <2 x float> %plBv, i32 0
  %40 = extractelement <2 x float> %plBv, i32 1''')

    # ------------------------------------------- generated hit and miss
    # The whole of the generated shader set for pattern 1. No any-hit: with
    # FORCE_OPAQUE traversal is entirely fixed function.
    sub('''; Function Attrs: nounwind readnone
declare i32 @dx.op.threadId.i32(i32, i32) #0''',
        '''define void @ClosestHit(%struct.Payload* noalias nocapture %p, %struct.BuiltInTriangleIntersectionAttributes* nocapture readonly %attr) #1 {
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

; Function Attrs: nounwind readonly
declare float @dx.op.rayTCurrent.f32(i32) #2

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure(i32, %struct.RaytracingAccelerationStructure) #2

; Function Attrs: nounwind readonly
declare %dx.types.Handle @"dx.op.createHandleForLib.class.RWStructuredBuffer<Result>"(i32, %"class.RWStructuredBuffer<Result>") #2

; Function Attrs: nounwind readonly
declare %dx.types.Handle @dx.op.createHandleForLib.CB(i32, %CB) #2''')

    # Drop the declarations that no longer have callers. A lib must not even
    # declare the rayQuery opcodes.
    for decl in [
        '\n; Function Attrs: nounwind\ndeclare i32 @dx.op.allocateRayQuery(i32, i32) #1\n',
        '\n; Function Attrs: nounwind\ndeclare void @dx.op.rayQuery_TraceRayInline(i32, i32, %dx.types.Handle, i32, i32, float, float, float, float, float, float, float, float) #1\n',
        '\n; Function Attrs: nounwind readonly\ndeclare float @dx.op.rayQuery_StateVector.f32(i32, i32, i8) #2\n',
        '\n; Function Attrs: nounwind readonly\ndeclare float @dx.op.rayQuery_StateScalar.f32(i32, i32) #2\n',
        '\n; Function Attrs: nounwind readonly\ndeclare i32 @dx.op.rayQuery_StateScalar.i32(i32, i32) #2\n',
        '\n; Function Attrs: nounwind\ndeclare i1 @dx.op.rayQuery_Proceed.i1(i32, i32) #1\n',
        '\n; Function Attrs: nounwind readonly\ndeclare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #2\n',
    ]:
        sub(decl, '\n')

    # ------------------------------------------------------------ metadata
    sub('!dx.entryPoints = !{!13}',
        '!dx.typeAnnotations = !{!13}\n!dx.entryPoints = !{!21, !23, !26, !28}')
    sub('!3 = !{!"cs", i32 6, i32 5}', '!3 = !{!"lib", i32 6, i32 5}')

    # Resource records point at the globals now, and carry their names.
    sub('!6 = !{i32 0, %struct.RaytracingAccelerationStructure* undef, !"", i32 0, i32 0, i32 1, i32 16, i32 0, !7}',
        '!6 = !{i32 0, %struct.RaytracingAccelerationStructure* @scene, !"scene", i32 0, i32 0, i32 1, i32 16, i32 0, !7}')
    sub('!9 = !{i32 0, %"class.RWStructuredBuffer<Result>"* undef, !"", i32 0, i32 0, i32 1, i32 12, i1 false, i1 false, i1 false, !10}',
        '!9 = !{i32 0, %"class.RWStructuredBuffer<Result>"* @outBuf, !"outBuf", i32 0, i32 0, i32 1, i32 12, i1 false, i1 false, i1 false, !10}')
    sub('!12 = !{i32 0, %CB* undef, !"", i32 0, i32 0, i32 1, i32 32, null}',
        '!12 = !{i32 0, %CB* @CB, !"CB", i32 0, i32 0, i32 1, i32 32, null}')

    # The compute entry record and its numthreads go; a library gets a
    # resource-only record plus one record per export. Tags, confirmed against
    # the DXC-built library in the recon: 8 shader kind (7 raygen, 10 closesthit,
    # 11 miss), 6 payload bytes, 7 attribute bytes, 5 auto binding space.
    sub('''!13 = !{void ()* @main, !"main", null, !4, !14}
!14 = !{i32 0, i64 33554448, i32 4, !15}
!15 = !{i32 8, i32 8, i32 1}''',
        '''!13 = !{i32 1, void ()* @RayGen, !14, void (%struct.Payload*, %struct.BuiltInTriangleIntersectionAttributes*)* @ClosestHit, !17, void (%struct.Payload*)* @Miss, !20}
!14 = !{!15}
!15 = !{i32 1, !16, !16}
!16 = !{}
!17 = !{!15, !18, !19}
!18 = !{i32 2, !16, !16}
!19 = !{i32 0, !16, !16}
!20 = !{!15, !18}
!21 = !{null, !"", null, !4, !22}
!22 = !{i32 0, i64 16}
!23 = !{void (%struct.Payload*, %struct.BuiltInTriangleIntersectionAttributes*)* @ClosestHit, !"ClosestHit", null, null, !24}
!24 = !{i32 8, i32 10, i32 6, i32 16, i32 7, i32 8, i32 5, !25}
!25 = !{i32 0}
!26 = !{void (%struct.Payload*)* @Miss, !"Miss", null, null, !27}
!27 = !{i32 8, i32 11, i32 6, i32 16, i32 5, !25}
!28 = !{void ()* @RayGen, !"RayGen", null, null, !29}
!29 = !{i32 8, i32 7, i32 5, !25}''')

    io.open(DST, "w", encoding="utf-8", newline="\n").write(s)
    print("wrote", DST)


if __name__ == "__main__":
    main()
