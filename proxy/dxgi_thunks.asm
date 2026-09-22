; ---------------------------------------------------------------------------
; dxgi_thunks.asm - tail-jump thunks for the dxgi.dll exports this proxy does
; not need to touch.
;
; Seventeen of dxgi.dll's twenty exports have no public signature, and none of
; them are interesting to this project. A tail jump does not need a signature:
; on x64 the arguments are already in rcx/rdx/r8/r9, xmm0-3 and on the caller's
; stack, and jumping straight to the real function leaves all of that, plus the
; return address, exactly as the caller set it up.
;
; Identical in shape to d3d12_thunks.asm. See that file for why the common
; helper always spills the argument registers.
;
; Adding another one is three edits: a THUNK line here, an entry in
; g_thunkNames in dxgi_proxy.cpp, and the ordinal in dxgi_proxy.def. The index
; order must match g_thunkNames.
; ---------------------------------------------------------------------------

EXTERN DxgiResolveThunk:PROC        ; void* DxgiResolveThunk(unsigned index)

.CODE

; ---------------------------------------------------------------------------
; DxgiThunkCommon - r10 = thunk index. Never called directly, only jumped to.
;
; Stack layout after the prologue (rsp is 16-byte aligned here):
;   [rsp+00h..1Fh]  shadow space for DxgiResolveThunk
;   [rsp+20h..3Fh]  saved rcx, rdx, r8, r9
;   [rsp+40h]       saved r10 (the index)
;   [rsp+50h..8Fh]  saved xmm0..xmm3
; ---------------------------------------------------------------------------
DxgiThunkCommon PROC FRAME
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
    call    DxgiResolveThunk
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
DxgiThunkCommon ENDP

THUNK MACRO fname, idx
fname PROC
    mov     r10d, idx
    jmp     DxgiThunkCommon
fname ENDP
ENDM

; Index order must match g_thunkNames in dxgi_proxy.cpp.
THUNK ApplyCompatResolutionQuirking,     0
THUNK CompatString,                      1
THUNK CompatValue,                       2
THUNK DXGID3D10CreateDevice,             3
THUNK DXGID3D10CreateLayeredDevice,      4
THUNK DXGID3D10GetLayeredDeviceSize,     5
THUNK DXGID3D10RegisterLayers,           6
THUNK DXGIDeclareAdapterRemovalSupport,  7
THUNK DXGIDisableVBlankVirtualization,   8
THUNK DXGIDumpJournal,                   9
THUNK DXGIGetDebugInterface1,            10
THUNK DXGIReportAdapterConfiguration,    11
THUNK PIXBeginCapture,                   12
THUNK PIXEndCapture,                     13
THUNK PIXGetCaptureState,                14
THUNK SetAppCompatStringPointer,         15
THUNK UpdateHMDEmulationStatus,          16

END
