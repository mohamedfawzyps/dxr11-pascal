; ---------------------------------------------------------------------------
; d3d12_thunks.asm - tail-jump thunks for d3d12.dll exports whose signatures
; are not public.
;
; d3d12SDKLayers.dll imports three functions from "d3d12.dll" BY NAME:
;   D3D12CoreCreateLayeredDevice, D3D12CoreGetLayeredDeviceSize,
;   D3D12CoreRegisterLayers
; Our proxy is the module named d3d12.dll in the app directory, so it has to
; provide them or the debug layer cannot load.
;
; Their signatures are not documented, so we cannot write C forwarders without
; guessing. A tail jump does not need the signature: on x64 the arguments are
; already in rcx/rdx/r8/r9, xmm0-3 and on the caller's stack, and jumping
; straight to the real function leaves all of that, plus the return address,
; exactly as the caller set it up. The real function returns to our caller
; directly.
;
; Each thunk loads an index into r10 (volatile, never an argument register) and
; jumps to the common helper, which resolves the target through the C++ side
; and tail-jumps to it. The helper always spills and restores the argument
; registers, even on the already-resolved path. That costs a few instructions
; on a call made a handful of times per process, and buys a single, ordinary
; prologue that x64 unwind info can describe.
;
; Adding another one is two lines: a THUNK macro line here, an entry in the
; C++ resolver table, and the ordinal in d3d12_proxy.def.
; ---------------------------------------------------------------------------

EXTERN Dxr11ResolveThunk:PROC       ; void* Dxr11ResolveThunk(unsigned index)

.CODE

; ---------------------------------------------------------------------------
; Dxr11ThunkCommon - r10 = thunk index. Never called directly, only jumped to.
;
; Stack layout after the prologue (rsp is 16-byte aligned here):
;   [rsp+00h..1Fh]  shadow space for Dxr11ResolveThunk
;   [rsp+20h..3Fh]  saved rcx, rdx, r8, r9
;   [rsp+40h]       saved r10 (the index)
;   [rsp+50h..8Fh]  saved xmm0..xmm3
; ---------------------------------------------------------------------------
Dxr11ThunkCommon PROC FRAME
    sub     rsp, 98h
    .allocstack 98h
    .endprolog

    mov     qword ptr [rsp+20h], rcx
    mov     qword ptr [rsp+28h], rdx
    mov     qword ptr [rsp+30h], r8
    mov     qword ptr [rsp+38h], r9
    mov     qword ptr [rsp+40h], r10
    movaps  xmmword ptr [rsp+50h], xmm0
    movaps  xmmword ptr [rsp+60h], xmm1
    movaps  xmmword ptr [rsp+70h], xmm2
    movaps  xmmword ptr [rsp+80h], xmm3

    movzx   ecx, r10b
    call    Dxr11ResolveThunk
    mov     r11, rax                    ; r11 is volatile and not an arg register

    movaps  xmm3, xmmword ptr [rsp+80h]
    movaps  xmm2, xmmword ptr [rsp+70h]
    movaps  xmm1, xmmword ptr [rsp+60h]
    movaps  xmm0, xmmword ptr [rsp+50h]
    mov     r10, qword ptr [rsp+40h]
    mov     r9,  qword ptr [rsp+38h]
    mov     r8,  qword ptr [rsp+30h]
    mov     rdx, qword ptr [rsp+28h]
    mov     rcx, qword ptr [rsp+20h]

    add     rsp, 98h

    test    r11, r11
    jz      short no_target
    jmp     r11                         ; tail jump: the real function returns
                                        ; straight to our caller
no_target:
    mov     eax, 80004005h              ; E_FAIL
    ret
Dxr11ThunkCommon ENDP

THUNK MACRO fname, idx
fname PROC
    mov     r10d, idx
    jmp     Dxr11ThunkCommon
fname ENDP
ENDM

; Index order must match g_thunkNames in d3d12_proxy.cpp.
THUNK D3D12CoreCreateLayeredDevice,  0
THUNK D3D12CoreGetLayeredDeviceSize, 1
THUNK D3D12CoreRegisterLayers,       2

END
