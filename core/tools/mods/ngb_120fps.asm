; Ninja Gaiden Black (5443000D.5) 120 fps: the header page past the headers
; (sizeof_headers 0xAF8 -> 0x1000).
;
; The game steps once a frame (fixed: it never reads the vblanks a frame
; took, 0xC05FD4). With the vblank at 120 Hz its logic keeps 60 updates a
; second: a logic frame is the first main-loop pass of each 60 Hz tick (the
; game's vblank count 0xD21738, bumped by its vblank callback 0x30E050,
; halved), decided where the main loop reads the controllers (0x30E1E1).
; Only logic frames read the controllers, count the rumble down and bump the
; frame counter (0xD21734). The game runs as fiber tasks (18 slots of 0x20
; at 0xD21C00: +0 flags, bit0 alive, bit5 paused; +4 frames to wait; +8
; fiber; +0xC entry; the current slot 0xD21E4C; scheduler 0x30C9D0): D3D's
; draws mark the current slot in DRAWN, and on the other frames the
; scheduler runs only the tasks that drew (a slot whose entry changed starts
; unmarked).
;
; Blending: the scene task (entry 0x1CB9A0) builds each frame's draw list
; from the camera (eye, target, angles, fov at 0x977E00; its matrices built
; by 0x42690) and the 64 objects at 0x4B4E50 (0x2800 apart: 35 bone matrices
; at +0, model space, then the world position at +0x8C0, three copies). On
; a logic frame, when the scheduler switches to it, those are taken (C) and
; replaced by the halfway values between the last logic frame's (P) and C;
; when it switches back, C goes back and becomes P. A logic frame shows half
; a step back, the frame after it the step itself: 120 even frames.
;
; The 2D: the game task (slot 13, 0x206270) fills the sprite list (0x8F7DF0)
; and queues three draw callbacks with 0x221210; the last (0x2213C0) empties
; the sprites and frees the 2D draw list (0xC0CF10). On the frames in between
; 0x221210 runs in its place, and on logic frames that last callback leaves
; the 2D for them (emptied at the next logic frame if none came between).
BITS 32
ORG 0x10B00

VBLANKS     equ 0xD21738        ; the game's vblank count
COUNTER     equ 0xD21734        ; its frame counter
SLOT        equ 0xD21E4C        ; the current task's slot (-1: the main fiber)
NSLOTS      equ 18
CTRL_POLL   equ 0x30D990        ; the controllers' update
CTRL_EDGE   equ 0x30DA60        ; their presses
RUMBLE      equ 0x30DE40        ; the rumble countdown
SKIP        equ 0x30CA8C        ; the scheduler: next slot
RESUME      equ 0x30CA56        ; the scheduler: the slot's wait countdown
SWITCH      equ 0x32A293        ; SwitchToFiber (stdcall)
SCENE       equ 0x1CB9A0        ; the scene task's entry
CAM_BUILD   equ 0x42690         ; the camera's matrices from CAM
CAM         equ 0x977E00        ; eye +0, target +0x10, angles +0x20 +0x24, +0x28, roll +0x2C, fov +0x30
OBJS        equ 0x4B4E50
OBJSZ       equ 0x2800
NOBJ        equ 64
OBJB        equ 0x8F0           ; what an object blends: 35 matrices, the position
POS         equ 0x8C0
NBONES      equ 35
MM_ALLOC    equ 0x3E1428        ; kernel thunk: MmAllocateContiguousMemory
DRAWVUP     equ 0x3C98C0
DRAWV       equ 0x3C9A70
DRAWIV      equ 0x3C9B40
DEVEND      equ 0x3C9E20
RUNPB       equ 0x3CAFC0
COPYRECTS   equ 0x3C6E10
GAMETASK    equ 0x206270        ; the game task (slot 13): its 2D (HUD) goes in the draw queue by
HUD_QUEUE   equ 0x221210        ; this, three callbacks (2D sprites at 0x8F7DF0), no state of its own
HUD_RESET   equ 0x2213C0        ; the last of them: the sprites and the draw list emptied
HUD_END     equ 0x3256D0        ; its end (jumped to)
HUD_FREE    equ 0x2228A0        ; the draw list freed (esi: the list)
HUD_LIST    equ 0xC0CF10        ; the 2D draw list
SPRITES_N   equ 0x8F7DFC        ; the 2D sprites' count

; the buffer
B_PFRAME    equ 0x00            ; the frame counter P was taken at
B_PVALID    equ 0x04
B_DOING     equ 0x08            ; C is in the objects' place (the scene runs)
B_ENT       equ 0x10            ; each slot's task entry when last checked (18 words)
B_CAMP      equ 0x60            ; the camera: P, C (0x34 bytes)
B_CAMC      equ 0xA0
B_P         equ 0x100           ; the objects: P, C (64 * 0x8F0 each)
B_C         equ B_P + NOBJ * OBJB
BUFSZ       equ B_C + NOBJ * OBJB

; ---- the main loop's controller calls (0x30E1E1): a logic frame reads them
input_hook:
    mov eax, [VBLANKS]
    shr eax, 1
    cmp eax, [lasttick]
    je .other
    mov [lasttick], eax
    mov dword [parity], 0
    cmp byte [hudkeep], 0       ; the last logic frame's 2D still kept (no frame in
    je .poll                    ; between came): empty it now
    mov byte [hudkeep], 0
    mov dword [SPRITES_N], 0
    push esi
    mov esi, HUD_LIST
    call HUD_FREE
    pop esi
.poll:
    call CTRL_POLL
    call CTRL_EDGE
    jmp RUMBLE
.other:
    mov dword [parity], 1
    ret

; ---- the scheduler's slot check (0x30CA4F: test al, 0x20 / jne SKIP /
; mov eax, [esi+4]): on the other frames a task that never drew is skipped
sched_hook:
    test al, 0x20
    jnz .skip
    push eax
    push edx
    mov eax, [SLOT]
    mov edx, [buf]              ; a new task in the slot (its entry changed): not marked
    test edx, edx
    jz .same
    lea edx, [edx+B_ENT+eax*4]
    push ecx
    mov ecx, [esi+0xC]
    cmp ecx, [edx]
    je .known
    mov [edx], ecx
    btr [drawn], eax
.known:
    pop ecx
.same:
    cmp dword [parity], 0
    je .run
    bt [drawn], eax
    jc .run
%ifdef DEBUGMASK
    bt [0x10FFC], eax           ; debug: the slots set in this word (poke it) run too
    jc .run
%endif
    pop edx
    pop eax
    cmp dword [esi+0xC], GAMETASK   ; in the game task's place, its 2D (the HUD)
    jne .skip
    pushad
    call HUD_QUEUE
    popad
.skip:
    jmp SKIP
.run:
    pop edx
    pop eax
    mov eax, [esi+4]
    jmp RESUME

; ---- the frame counter's load (0x30E316): bumped on logic frames only
count_hook:
    mov ebx, [COUNTER]
    cmp dword [parity], 0
    jne .ret
    inc ebx
.ret:
    ret

; ---- D3D's draws mark the current slot (their first instructions moved here)
mark:
    push eax
    mov eax, [SLOT]
    cmp eax, NSLOTS
    jae .no
    bts [drawn], eax
.no:
    pop eax
    ret
drawvup_hook:
    call mark
    push ebp
    mov ebp, esp
    sub esp, 0x14
    jmp DRAWVUP + 6
drawv_hook:
    call mark
    push ebx
    push ebp
    mov ebp, [0x3D83C8]
    jmp DRAWV + 8
drawiv_hook:
    call mark
    push ebp
    mov ebp, esp
    sub esp, 8
    jmp DRAWIV + 6
devend_hook:
    call mark
    push esi
    mov esi, [0x3D83C8]
    jmp DEVEND + 7
runpb_hook:
    call mark
    sub esp, 0x30
    push ebx
    push ebp
    jmp RUNPB + 5
copyrects_hook:
    call mark
    sub esp, 0x34
    push ebx
    push ebp
    jmp COPYRECTS + 5

; ---- the game task's last 2D callback (0x2213C0: the sprite count to 0, the
; draw list freed, then 0x3256D0): on a logic frame the 2D stays, for the
; frame after to draw again (its callbacks queued in the game task's place)
hud_reset:
    cmp dword [parity], 0
    jne .go
    mov byte [hudkeep], 1
    jmp HUD_END
.go:
    mov byte [hudkeep], 0
    push esi
    mov esi, HUD_LIST
    jmp HUD_RESET + 6

; ---- the scheduler's switch to a task (0x30CA66: mov eax, [esi+8] / push
; eax / call SWITCH): around the scene task, the blend
resume_hook:
    cmp dword [esi+0xC], SCENE
    jne .plain
    pushad
    call scene_pre
    popad
    push dword [esi+8]
    call SWITCH
    pushad
    call scene_post
    popad
    ret
.plain:
    push dword [esi+8]
    call SWITCH
    ret

scene_pre:
    mov ebp, [buf]
    test ebp, ebp
    jnz .have
    push BUFSZ
    call dword [MM_ALLOC]
    test eax, eax
    jz .ret                     ; no memory yet: again at the next frame
    mov ebp, eax
    mov edi, eax                ; the variables: zeros
    mov ecx, B_P / 4
    xor eax, eax
    rep stosd
    mov [buf], ebp
.have:
    cmp dword [parity], 0
    jne .other
    ; a logic frame: C = now; halfway when P is the frame before's
    lea edi, [ebp+B_CAMC]
    mov esi, CAM
    mov ecx, 13
    rep movsd
    xor ebx, ebx
.take:
    mov esi, ebx
    imul esi, esi, OBJSZ
    add esi, OBJS
    mov edi, ebx
    imul edi, edi, OBJB
    lea edi, [ebp+B_C+edi]
    mov ecx, OBJB / 4
    rep movsd
    inc ebx
    cmp ebx, NOBJ
    jb .take
    mov byte [ebp+B_DOING], 1
    mov eax, [COUNTER]
    dec eax
    cmp eax, [ebp+B_PFRAME]
    jne .build
    cmp dword [ebp+B_PVALID], 0
    je .build
    call cam_half
    xor ebx, ebx
.objs:
    call obj_half
    inc ebx
    cmp ebx, NOBJ
    jb .objs
.build:
.other:
    call CAM_BUILD              ; the camera's matrices (as its task leaves them)
.ret:
    ret

scene_post:
    mov ebp, [buf]
    test ebp, ebp
    jz .ret
    cmp byte [ebp+B_DOING], 0
    je .ret
    mov byte [ebp+B_DOING], 0
    ; C back in place; P = C
    lea esi, [ebp+B_CAMC]
    mov edi, CAM
    mov ecx, 13
    rep movsd
    lea esi, [ebp+B_CAMC]
    lea edi, [ebp+B_CAMP]
    mov ecx, 13
    rep movsd
    xor ebx, ebx
.put:
    mov esi, ebx
    imul esi, esi, OBJB
    lea esi, [ebp+B_C+esi]
    mov edi, ebx
    imul edi, edi, OBJSZ
    add edi, OBJS
    mov ecx, OBJB / 4
    rep movsd
    inc ebx
    cmp ebx, NOBJ
    jb .put
    lea esi, [ebp+B_C]
    lea edi, [ebp+B_P]
    mov ecx, NOBJ * OBJB / 4
    rep movsd
    mov eax, [COUNTER]
    mov [ebp+B_PFRAME], eax
    mov dword [ebp+B_PVALID], 1
    call CAM_BUILD
.ret:
    ret

; the camera halfway (P at B_CAMP, C at B_CAMC, into CAM); a cut: C
cam_half:
    lea esi, [ebp+B_CAMP]
    lea edx, [ebp+B_CAMC]
    xor ecx, ecx                ; the eye, then the target: a cut if either jumps
.cut:
    movups xmm0, [esi+ecx]
    movups xmm1, [edx+ecx]
    subps xmm0, xmm1
    mulps xmm0, xmm0
    movaps xmm1, xmm0
    shufps xmm1, xmm1, 0x55
    addss xmm0, xmm1
    movhlps xmm1, xmm0
    addss xmm0, xmm1
    comiss xmm0, [c_cut]
    ja .ret
    add ecx, 16
    cmp ecx, 32
    jb .cut
    mov edi, CAM
    xor ecx, ecx
.f:                             ; eye, target: halfway (w stays)
    movss xmm0, [esi+ecx*4]
    addss xmm0, [edx+ecx*4]
    mulss xmm0, [c_half]
    movss [edi+ecx*4], xmm0
    inc ecx
    cmp ecx, 8
    jb .f
    mov ecx, 8                  ; the angles: 16-bit turns, the short way
    call .angle
    mov ecx, 9
    call .angle
    mov ecx, 11
    call .angle
    movss xmm0, [esi+10*4]
    addss xmm0, [edx+10*4]
    mulss xmm0, [c_half]
    movss [edi+10*4], xmm0
    movss xmm0, [esi+12*4]      ; fov
    addss xmm0, [edx+12*4]
    mulss xmm0, [c_half]
    movss [edi+12*4], xmm0
.ret:
    ret
.angle:
    mov eax, [edx+ecx*4]
    sub eax, [esi+ecx*4]
    movsx eax, ax
    sar eax, 1
    add eax, [esi+ecx*4]
    mov [edi+ecx*4], eax
    ret

; object ebx halfway (P, C in the buffer, into its place); a jump: C
obj_half:
    mov esi, ebx
    imul esi, esi, OBJB
    lea edx, [ebp+B_C+esi]
    lea esi, [ebp+B_P+esi]
    mov edi, ebx
    imul edi, edi, OBJSZ
    add edi, OBJS
    movups xmm0, [esi+POS]
    movups xmm1, [edx+POS]
    subps xmm0, xmm1
    mulps xmm0, xmm0
    movaps xmm1, xmm0
    shufps xmm1, xmm1, 0x55
    addss xmm0, xmm1
    movhlps xmm1, xmm0
    addss xmm0, xmm1
    comiss xmm0, [c_jump]
    ja .ret
    movups xmm7, [c_halves]
    xor ecx, ecx
.f:
    movups xmm0, [esi+ecx]
    movups xmm1, [edx+ecx]
    addps xmm0, xmm1
    mulps xmm0, xmm7
    movups [edi+ecx], xmm0
    add ecx, 16
    cmp ecx, OBJB
    jb .f
    ; each bone's rotation rows back to their length in C
    xor ecx, ecx
.row:
    mov eax, ecx                ; rows 0-2 of each matrix (not the translation)
    and eax, 0x30
    cmp eax, 0x30
    je .next
    movups xmm0, [edi+ecx]
    movups xmm1, [edx+ecx]
    mulps xmm0, xmm0
    mulps xmm1, xmm1
    movaps xmm2, xmm0           ; |mid|^2
    shufps xmm2, xmm2, 0x55
    addss xmm0, xmm2
    movaps xmm2, xmm0
    movhlps xmm2, xmm2
    addss xmm0, xmm2
    movaps xmm2, xmm1           ; |C|^2
    shufps xmm2, xmm2, 0x55
    addss xmm1, xmm2
    movaps xmm2, xmm1
    movhlps xmm2, xmm2
    addss xmm1, xmm2
    comiss xmm0, [c_tiny]
    jb .next
    divss xmm1, xmm0
    sqrtss xmm1, xmm1           ; |C| / |mid|
    shufps xmm1, xmm1, 0
    movups xmm0, [edi+ecx]
    mulps xmm0, xmm1
    movups [edi+ecx], xmm0
.next:
    add ecx, 16
    cmp ecx, NBONES * 64
    jb .row
.ret:
    ret

align 4
c_halves:   dd 0.5, 0.5, 0.5, 0.5
c_half:     dd 0.5
c_cut:      dd 9.0              ; the eye or the target moved 3 or more: a cut
c_jump:     dd 4.0              ; an object moved 2 or more: no blend
c_tiny:     dd 1.0e-6

; the variables (zeros in the file, not patched)
align 4
buf:        dd 0
parity:     dd 0
lasttick:   dd 0
hudkeep:    dd 0
drawn:      dd 0
