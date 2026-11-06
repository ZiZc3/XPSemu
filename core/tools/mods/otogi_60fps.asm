; Otogi (46530002.2) 60 fps: the header page past the headers
; (sizeof_headers 0x9F8 -> 0x1000).
;
; The stage loop in 0x42DE0 renders (0x428A0) and presents (0x15280), then
; steps the game clock by one frame and runs the logic (0x41BB0, 0x42770).
; Its present waits one vblank instead of two (D3D's presentation interval,
; 0x10512C, for that Swap only: menus and cutscenes keep 30), and every
; other frame goes back to the render without the clock step and the logic:
; the logic keeps 30 steps a second (STEP counts them).
;
; The first render after each step draws halfway between the last two
; steps: the camera from its start, each model from its first DrawModel
; (0x23BC0, or its plain twin 0x22640) on (its world matrix at +0 and its N
; local node matrices at [+0x40], N = [[+0x48]+0x54]: (P + C) / 2 of its
; values at its last two steps, taken there), until the render's end puts
; C back. The render builds some
; models itself from others (attachments from a blended parent, the sky
; from the camera): at their draw in the render after, without blending,
; they differ from that C, and from then on they follow what they come
; from (derived).
BITS 32
ORG 0x10A00

INTERVAL    equ 0x10512C        ; D3D: the presentation interval Swap uses
SWAP_WRAP   equ 0x15280         ; the stage loop's present
RENDER      equ 0x428A0         ; the stage render
LOOP_TOP    equ 0x43186         ; the stage loop: pause check, then the render
PAUSE_FLAG  equ 0x1D80E0
DRAW_BODY   equ 0x23BC5         ; DrawModel past its first two instructions
DRAW2_BODY  equ 0x22645         ; the same for its plain twin
MM_ALLOC    equ 0x173E00        ; kernel thunk: MmAllocateContiguousMemory
CAM_VIEW    equ 0x1386B20       ; the camera: view matrix (64 bytes), two
CAM_A       equ 0x1386AB4       ; points (12 bytes each)
CAM_B       equ 0x1386B08

; the buffer
STEP        equ 0x00
BLEND       equ 0x04            ; in a render that blends
BUMP        equ 0x08            ; the data area's next free byte (offset)
CAMSTEP     equ 0x0C            ; the camera's step, P valid
CAMOK       equ 0x14
LASTSTEP    equ 0x18            ; the step of the last render
CAMP        equ 0x20            ; P, C, halfway: view, A, B (88 bytes each)
CAMC        equ 0x80
CAMH        equ 0xE0
TAB         equ 0x200           ; models: 1024 entries of 32 bytes
ENTRIES     equ 1024
DATA        equ TAB + ENTRIES * 32
BUFSZ       equ 0x100000
NEVER       equ 0x80000000      ; a step nothing has seen
; entry: +0 model, +4 its mesh, +8 bytes (64 + N * 64), +12 step, +16 its
; nodes' address, +20 P's offset (C follows), +24 P valid, +25 derived,
; +26 halfway now, +27 checked since the step, +28 the bytes P has room for

; ---- the stage loop's present (0x431A6): one vblank
stage_swap:
    push dword [INTERVAL]
    mov dword [INTERVAL], 0x80000001
    call SWAP_WRAP
    pop dword [INTERVAL]
    ret

; ---- after it (0x431AB, was cmp byte [PAUSE_FLAG], bl)
stage_step:
    xor byte [parity], 1
    jz .logic
    mov dword [esp], LOOP_TOP   ; a frame without logic: render again
    ret
.logic:
    mov eax, [buf]
    cmp eax, 1
    jbe .no
    inc dword [eax+STEP]
.no:
    cmp byte [PAUSE_FLAG], bl
    ret

; ---- the render call (0x4318F)
render_hook:
    pushad
    mov ebp, [buf]
    test ebp, ebp
    jnz .have
    push BUFSZ
    call dword [MM_ALLOC]
    test eax, eax
    jnz .got
    inc eax                     ; 1: no memory, off
    jmp .keep
.got:
    mov edi, eax                ; the variables and the table: zeros
    mov ecx, DATA / 4
    push eax
    xor eax, eax
    rep stosd
    pop eax
    mov dword [eax+CAMSTEP], NEVER
    mov dword [eax+LASTSTEP], NEVER
.keep:
    mov [buf], eax
    mov ebp, eax
.have:
    cmp ebp, 1
    jbe .plain
    mov eax, [ebp+STEP]         ; the first render after a step?
    cmp eax, [ebp+LASTSTEP]
    je .plain
    mov [ebp+LASTSTEP], eax
    call cam_step
    mov byte [ebp+BLEND], 1
    popad
    call RENDER
    pushad
    mov ebp, [buf]
    mov byte [ebp+BLEND], 0
    call models_back
    cmp byte [ebp+CAMOK], 0
    je .done
    lea esi, [ebp+CAMC]
    call cam_put
.done:
    popad
    ret
.plain:
    popad
    jmp RENDER

; the camera: P = C, C = now, the camera halfway
cam_step:
    mov eax, [ebp+STEP]
    dec eax
    cmp eax, [ebp+CAMSTEP]
    sete byte [ebp+CAMOK]
    inc eax
    mov [ebp+CAMSTEP], eax
    lea esi, [ebp+CAMC]
    lea edi, [ebp+CAMP]
    mov ecx, 22
    rep movsd
    lea edi, [ebp+CAMC]
    call cam_get
    cmp byte [ebp+CAMOK], 0
    je .ret
    lea esi, [ebp+CAMP]
    lea edx, [ebp+CAMC]
    lea edi, [ebp+CAMH]
    mov ecx, 22
    call half
    lea esi, [ebp+CAMH]
    call cam_put
.ret:
    ret

; the camera to edi / from esi (88 bytes)
cam_get:
    mov esi, CAM_VIEW
    mov ecx, 16
    rep movsd
    mov esi, CAM_A
    movsd
    movsd
    movsd
    mov esi, CAM_B
    movsd
    movsd
    movsd
    ret
cam_put:
    mov edi, CAM_VIEW
    mov ecx, 16
    rep movsd
    mov edi, CAM_A
    movsd
    movsd
    movsd
    mov edi, CAM_B
    movsd
    movsd
    movsd
    ret

; ecx floats: edi = (esi + edx) / 2
half:
    fld dword [esi]
    fadd dword [edx]
    fmul dword [c_half]
    fstp dword [edi]
    add esi, 4
    add edx, 4
    add edi, 4
    dec ecx
    jnz half
    ret

; ---- DrawModel (0x23BC0) and its plain twin (0x22640), jumped to
draw_hook:
    call drawn
    push esi
    mov esi, [esp+8]
    jmp DRAW_BODY
draw2_hook:
    call drawn
    push esi
    mov esi, [esp+8]
    jmp DRAW2_BODY

drawn:
    pushad
    mov ebx, [esp+0x28]
    mov ebp, [buf]
    cmp ebp, 1
    jbe .go
    test ebx, ebx
    jz .go
    call on_draw
.go:
    popad
    ret

; edi = the entry of model ebx, or the empty one it gets, or 0 (8 probes)
find:
    mov edi, ebx
    shr edi, 4
    and edi, ENTRIES - 1
    mov eax, 8
.probe:
    mov esi, edi
    shl esi, 5
    lea esi, [ebp+TAB+esi]
    cmp [esi], ebx
    je .hit
    cmp dword [esi], 0
    je .hit
    inc edi
    and edi, ENTRIES - 1
    dec eax
    jnz .probe
    xor edi, edi
    ret
.hit:
    mov edi, esi
    ret

; ZF set when model ebx still is what entry edi knows (mesh, nodes, size)
live:
    mov eax, [ebx+0x48]
    cmp eax, [edi+4]
    jne .ret
    mov eax, [ebx+0x40]
    cmp eax, [edi+16]
    jne .ret
    mov eax, [edi+4]
    mov eax, [eax+0x54]
    shl eax, 6
    add eax, 64
    cmp eax, [edi+8]
.ret:
    ret

; model ebx drawn
on_draw:
%ifdef DEBUG
    cmp byte [ebp+BLEND], 0
    je .nb
    inc dword [ebp+0x1D8]       ; draws in blended renders
.nb:
    inc dword [ebp+0x1E0]       ; ... that are not models (left below)
%endif
    mov ecx, [ebx+0x48]
    test ecx, ecx
    jz .ret
    cmp dword [ebx+0x40], 0
    je .ret
    mov edx, [ecx+0x54]
    cmp edx, 255
    ja .ret
%ifdef DEBUG
    dec dword [ebp+0x1E0]
%endif
    shl edx, 6
    add edx, 64                 ; edx = bytes
    call find
    test edi, edi
    jz .ret
    cmp [edi], ebx
    jne .new
    call live
    jne .changed
    cmp byte [edi+25], 0        ; derived: left alone
    jne .ret
    mov eax, [ebp+STEP]
    cmp byte [ebp+BLEND], 0
    je .check
    cmp eax, [edi+12]           ; drawn before in this render
    je .ret
    jmp step_model
.check:
    cmp eax, [edi+12]           ; the render after the blended one: is it
    jne .ret                    ; still C?
    cmp byte [edi+27], 0
    jne .ret
    mov byte [edi+27], 1
    mov esi, [edi+20]
    add esi, [edi+8]
    push edi
    lea edi, [ebp+DATA+esi]     ; C
    mov esi, ebx
    mov ecx, 16
    repe cmpsd
    jne .differs
    mov esi, [ebx+0x40]
    mov ecx, edx
    sub ecx, 64
    shr ecx, 2
    repe cmpsd
    je .same
.differs:
    mov eax, [esp]
    mov byte [eax+25], 1
%ifdef DEBUG
    mov eax, [ebp+0x13C]        ; a log: model, where it differs (dwords left)
    and eax, 15
    lea eax, [ebp+0x140+eax*8]
    mov [eax], ebx
    mov [eax+4], ecx
    inc dword [ebp+0x13C]
%endif
.same:
    pop edi
.ret:
    ret
.new:
    mov eax, [ebp+BUMP]         ; data for P and C
    lea esi, [eax+edx*2]
    cmp esi, BUFSZ - DATA
    ja .ret
    mov [ebp+BUMP], esi
    mov [edi], ebx
    mov [edi+20], eax
    mov [edi+28], edx
    jmp .init
.changed:
%ifdef DEBUG
    inc dword [ebp+0x1C]        ; changed models
    mov eax, [ebx+0x40]
    cmp eax, [edi+16]
    je .samenodes
    inc dword [ebp+0x1C0]       ; ... with other nodes
    mov [ebp+0x1C4], ebx
    mov [ebp+0x1C8], eax
.samenodes:
%endif
    cmp edx, [edi+28]
    jbe .init
    mov byte [edi+25], 1        ; no room for it: left alone
    ret
.init:
    mov [edi+4], ecx
    mov [edi+8], edx
    mov eax, [ebx+0x40]
    mov [edi+16], eax
    mov eax, [ebp+STEP]
    mov [edi+12], eax
    mov dword [edi+24], 0
    mov eax, [edi+20]
    add eax, edx
    lea edi, [ebp+DATA+eax]     ; C
    mov ecx, edx
    jmp model_get

; entry edi, model ebx (edx bytes), at its first draw in a blended render:
; P = C, C = now, the model halfway
step_model:
    dec eax
    cmp eax, [edi+12]
    sete byte [edi+24]
    inc eax
    mov [edi+12], eax
    mov byte [edi+27], 0
%ifdef DEBUG
    inc dword [ebp+0x1D0]       ; steps
    cmp byte [edi+24], 0
    je .nov
    inc dword [ebp+0x1D4]       ; ... with P valid
.nov:
%endif
    push edi
    mov esi, [edi+20]
    lea esi, [ebp+DATA+esi]     ; P
    mov ecx, edx
    mov edi, esi
    add esi, ecx                ; C
    shr ecx, 2
    rep movsd                   ; P = C (edi then at C)
    mov ecx, edx
    call model_get              ; C = now
    pop edi
    cmp byte [edi+24], 0
    je .ret
    mov esi, [edi+20]
    lea esi, [ebp+DATA+esi]     ; P
    lea edx, [esi+edx]          ; C
    fld dword [esi+48]          ; moved more than 3000 units: no blend
    fsub dword [edx+48]
    fmul st0, st0
    fld dword [esi+52]
    fsub dword [edx+52]
    fmul st0, st0
    faddp st1, st0
    fld dword [esi+56]
    fsub dword [edx+56]
    fmul st0, st0
    faddp st1, st0
    fcomp dword [c_jump2]
    fnstsw ax
    sahf
%ifdef DEBUG
    jbe .near
    inc dword [ebp+0x1DC]       ; teleports
    ret
.near:
%endif
    ja .ret
    jp .ret
%ifdef DEBUG
    inc dword [ebp+0x1CC]       ; blends
%endif
    mov byte [edi+26], 1
    mov ecx, [edi+8]
    shr ecx, 2
    push ecx
    mov edi, ebx                ; its matrix, then its nodes
    push 16
    pop ecx
    call half
    pop ecx
    sub ecx, 16
    jz .ret
    mov edi, [ebx+0x40]
    jmp half
.ret:
    ret

; after the blended render: each model halfway gets C back
models_back:
    lea edi, [ebp+TAB]
    mov ecx, ENTRIES
.loop:
    cmp byte [edi+26], 0
    je .next
    mov byte [edi+26], 0
    push ecx
    push edi
    mov ebx, [edi]
    call live
    jne .skip
    mov esi, [edi+20]
    add esi, [edi+8]
    lea esi, [ebp+DATA+esi]     ; C
    mov ecx, [edi+8]
    call model_put
.skip:
    pop edi
    pop ecx
.next:
    add edi, 32
    dec ecx
    jnz .loop
    ret

; the model ebx's matrices to edi / from esi (ecx bytes: 64 + N * 64)
model_get:
    push ecx
    mov esi, ebx
    mov ecx, 16
    rep movsd
    pop ecx
    sub ecx, 64
    shr ecx, 2
    mov esi, [ebx+0x40]
    rep movsd
    ret
model_put:
    push ecx
    mov edi, ebx
    mov ecx, 16
    rep movsd
    pop ecx
    sub ecx, 64
    shr ecx, 2
    mov edi, [ebx+0x40]
    rep movsd
    ret

c_half:     dd 0.5
c_jump2:    dd 9000000.0
buf:        dd 0                ; the buffer (0: not yet, 1: none)
parity:     db 0
