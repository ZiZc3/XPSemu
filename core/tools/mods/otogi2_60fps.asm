; Otogi 2 (46530004.2) 60 fps: the header page past the headers
; (sizeof_headers 0xA08 -> 0x1000).
;
; The stage loop at 0x4BA50 runs the game timers and the update (0x114F0,
; 0x4BC10), renders (0x4BD70) and presents (Swap at 0x4BA91), then steps the
; game clock by one frame. Its present waits one vblank instead of two
; (D3D's presentation interval, 0x1EE404, for that Swap only: menus keep
; 30), and the frames go back to the render without the clock step, the
; timers and the update until the time since the last step (KeTickCount)
; comes to most of a 30th of a second: every other frame at 60 frames a
; second, more often below, so the game keeps its speed (STEP counts the
; steps).
;
; The first render after a step on time (a frame or more late: C) draws
; halfway between the last two steps: the camera from its start, each model
; from its first DrawModel (0x56EA0, or its plain twin 0x57170) on (its
; world matrix at +0, its N local node matrices at [+0x40] and the N skin
; matrices at [+0x44] when they follow, N = [[+0x48]+0x18]: (P + C) / 2 of
; its values at its last two steps, taken there), until the render's end
; puts C back. The render builds some models itself from others: at their
; draw in the render after, without blending, they differ from that C, and
; from then on they follow what they come from (derived).
BITS 32
ORG 0x10A10

INTERVAL    equ 0x1EE404        ; D3D: the presentation interval Swap uses
SWAP        equ 0x1DC950        ; D3D Swap (eax: flags), the stage loop's present
RENDER      equ 0x4BD70         ; the stage render
LOOP_SKIP   equ 0x4BA63         ; the stage loop past the update: exit check, render
PAUSE_FLAG  equ 0x237414
DRAW_BODY   equ 0x56EA6         ; DrawModel past its first three instructions
DRAW2_BODY  equ 0x57175         ; its plain twin past its first two
MM_ALLOC    equ 0x1F58EC        ; kernel thunk: MmAllocateContiguousMemory
KE_TICKS    equ 0x1F5950        ; kernel thunk: KeTickCount (ms)
CAM_VIEW    equ 0x9DFAE0        ; the camera: view matrix (64 bytes), two
CAM_A       equ 0x9DFA74        ; points (12 bytes each)
CAM_B       equ 0x9DFAC8

; the buffer
STEP        equ 0x00
BLEND       equ 0x04            ; in a render that blends
BUMP        equ 0x08            ; the data area's next free byte (offset)
CAMSTEP     equ 0x0C            ; the camera's step, P valid
FULL        equ 0x10            ; no room for a new model: start over
CAMOK       equ 0x14
LASTSTEP    equ 0x18            ; the step of the last render
LATE        equ 0x1C            ; the time past the last step (1/3 ms)
BLENDS      equ 0x1FC           ; the last step on time (byte)
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
; +26 blended now, +27 checked since the step, +28 the bytes P has room for

; ---- the stage loop's present (0x4BA91): one vblank
stage_swap:
    mov byte [presented], 1
    push dword [INTERVAL]
    mov dword [INTERVAL], 0x80000001
    call SWAP
    pop dword [INTERVAL]
    ret

; ---- after it (0x4BA96, was cmp byte [PAUSE_FLAG], bl)
stage_step:
    mov eax, [KE_TICKS]
    mov eax, [eax]
    mov ecx, [lasttick]
    mov [lasttick], eax
    cmp byte [presented], 0     ; the render left out (0x32240): the game's
    je .unpaced                 ; own pace, a step each time
    mov byte [presented], 0
    sub eax, ecx                ; the frame's ms (up to a second)
    cmp eax, 1000
    jb .ms
    mov eax, 1000
.ms:
%ifdef DEBUG
    inc dword [dbg_frames]
    add [dbg_ms], eax
    mov ecx, [buf]              ; the last 16 frames' ms
    cmp ecx, 1
    jbe .nd
    movzx edx, byte [ecx+0x1F8]
    mov [ecx+0x1E8+edx], al
    inc edx
    and edx, 15
    mov [ecx+0x1F8], dl
.nd:
%endif
    lea eax, [eax+eax*2]        ; in 1/3 ms: a step is 100
    add eax, [acc]
    cmp eax, 75                 ; three quarters of a step: a step now
    jge .logic
    mov [acc], eax
    mov dword [esp], LOOP_SKIP  ; a frame without logic: render again
    ret
.logic:
    sub eax, 100
    cmp eax, 800                ; at most 8 steps behind (one a frame)
    jle .keep
    mov eax, 800
    jmp .keep
.unpaced:
    xor eax, eax
.keep:
    mov [acc], eax
    mov ecx, [buf]
    cmp ecx, 1
    jbe .no
    inc dword [ecx+STEP]
    mov [ecx+LATE], eax
.no:
    cmp byte [PAUSE_FLAG], bl
    ret

; ---- the render call (0x4BA75)
render_hook:
    pushad
    mov ebp, [buf]
    test ebp, ebp
    jnz .have
    push BUFSZ
    call dword [MM_ALLOC]
    test eax, eax
    jz .plain                   ; no memory yet: again at the next render
    mov edi, eax                ; the variables and the table: zeros
    mov ecx, DATA / 4
    push eax
    xor eax, eax
    rep stosd
    pop eax
    mov dword [eax+CAMSTEP], NEVER
    mov dword [eax+LASTSTEP], NEVER
    mov [buf], eax
    mov ebp, eax
.have:
    cmp ebp, 1
    jbe .plain
    cmp byte [ebp+FULL], 0      ; the data area full: forget every model
    je .room
    lea edi, [ebp+TAB]
    mov ecx, ENTRIES * 8
    xor eax, eax
    rep stosd
    mov [ebp+BUMP], eax
    mov [ebp+FULL], al
.room:
    mov eax, [ebp+STEP]         ; the first render after a step?
    cmp eax, [ebp+LASTSTEP]
    je .plain
    mov [ebp+LASTSTEP], eax
    cmp dword [ebp+LATE], 35    ; the step on time (within 2/3 of a frame):
    setl byte [ebp+BLENDS]      ; halfway, else C
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
    mov cl, [ebp+BLENDS]
    and [ebp+CAMOK], cl
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

; ---- DrawModel (0x56EA0) and its plain twin (0x57170), jumped to
draw_hook:
    call drawn
    push ebx
    push edi
    mov edi, [esp+0xC]
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
    push edx
    push ecx
    call model_bytes
    cmp edx, [edi+8]
    pop ecx
    pop edx
.ret:
    ret

; edx = the bytes model ebx blends: its matrix, its N nodes, and the N skin
; matrices at [+0x44] when they follow the nodes (eax, ecx changed)
model_bytes:
    mov ecx, [ebx+0x48]
    mov edx, [ecx+0x18]
    shl edx, 6
    mov eax, [ebx+0x40]
    add eax, edx
    cmp eax, [ebx+0x44]
    jne .nodes
    add edx, edx
.nodes:
    add edx, 64
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
    cmp dword [ecx+0x18], 255
    ja .ret
%ifdef DEBUG
    dec dword [ebp+0x1E0]
%endif
    call model_bytes            ; edx = bytes
    mov ecx, [ebx+0x48]
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
    je .again
    jmp step_model
.again:
    cmp byte [edi+26], 0        ; blended at its first draw, its values
    je .ret                     ; rebuilt since (C again): blended again
    call same_c
    jne .ret
%ifdef DEBUG
    inc dword [dbg_again]
%endif
    jmp put_t
.check:
    cmp eax, [edi+12]           ; the render after the blended one: is it
    jne .ret                    ; still C?
    cmp byte [edi+27], 0
    jne .ret
    mov byte [edi+27], 1
    call same_c
    je .ret
    mov byte [edi+25], 1
%ifdef DEBUG
    mov eax, [ebp+0x13C]        ; a log: model, where it differs (dwords left)
    and eax, 15
    lea eax, [ebp+0x140+eax*8]
    mov [eax], ebx
    mov [eax+4], ecx
    inc dword [ebp+0x13C]
%endif
.ret:
    ret
.new:
    mov eax, [ebp+BUMP]         ; data for P and C
    lea esi, [eax+edx*2]
    cmp esi, BUFSZ - DATA
    jbe .room
    mov byte [ebp+FULL], 1      ; none left: the next render starts over
    ret
.room:
    mov [ebp+BUMP], esi
    mov [edi], ebx
    mov [edi+20], eax
    mov [edi+28], edx
    jmp .init
.changed:
%ifdef DEBUG
    inc dword [ebp+0x1E4]       ; changed models
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
    cmp byte [ebp+BLENDS], 0
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
    jmp put_t
.ret:
    ret

; entry edi, model ebx: the model halfway (its matrix, then its nodes)
put_t:
    mov byte [edi+26], 1
    mov ecx, [edi+8]
    mov esi, [edi+20]
    lea esi, [ebp+DATA+esi]     ; P
    lea edx, [esi+ecx]          ; C
    shr ecx, 2
    push ecx
    mov edi, ebx
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

; entry edi, model ebx (edx bytes): ZF set when the model holds C
same_c:
    push edi
    mov esi, [edi+20]
    add esi, [edi+8]
    lea edi, [ebp+DATA+esi]     ; C
    mov esi, ebx
    mov ecx, 16
    repe cmpsd
    jne .out
    mov esi, [ebx+0x40]
    mov ecx, edx
    sub ecx, 64
    shr ecx, 2                  ; (none: ZF set)
    repe cmpsd
.out:
    pop edi
    ret

; after the blended render: each model blended gets C back
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

align 4
c_half:     dd 0.5
c_jump2:    dd 9000000.0
buf:        dd 0                ; the buffer (0: not yet)
lasttick:   dd 0                ; KeTickCount at the last present
acc:        dd 0                ; the time since the last step (1/3 ms)
presented:  db 0                ; Swap since the last stage_step
%ifdef DEBUG
align 4
dbg_frames: dd 0
dbg_ms:     dd 0
dbg_again:  dd 0
%endif
