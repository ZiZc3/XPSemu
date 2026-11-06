; Halo (4D530004.9) 120 fps: objects and first-person arms drawn between ticks.
; Lives in the header page, over the logo bitmap (only the dashboard shows it)
; and past the headers (sizeof_headers 0xCC8 -> 0x1000).
;
; Halo ticks 30 times a second; with the limiter at one vblank a frame (the
; vblank at 120 Hz), three frames in four run no tick. t is how far between the
; last two ticks a frame is shown: a quarter tick apart, 1/4, 1/2, 3/4 and 1
; (the last tick) at 120 fps (halo_60fps.asm: 1/2 and 1 at 60).
; - Objects: before each tick their positions and node matrices are copied to
;   PREV. After the frame's ticks (game_time_update), when t < 1, the current
;   values go to CUR and the objects get PREV + (cur - PREV) * t for the
;   director, the observer (camera) and the render; after the frame (before
;   the vblank wait) CUR goes back.
; - First person: the render rebuilds each local player's arms every frame
;   (0xD77E0: the pose from the tick's animations, then the node matrices from
;   the pose and the render camera, 0xA3600). At that call the pose of the
;   last tick (LAST) blends into the current one: quaternions (sign-matched,
;   normalized), translations and scales; the pose comes back after the call.
BITS 32
ORG 0x10A14

OBJ_ARRAY   equ 0x2FC6AC        ; object header data array (data_t *)
FP_ARRAY    equ 0x276B48        ; first-person weapons: 4 local players of 0x1EA0
GAME_TIME   equ 0x2F8CA0        ; game_time_globals *: +0x0C ticks, +0x18 speed,
                                ; +0x1C leftover (s)
MM_ALLOC    equ 0x1D66D4        ; kernel thunk: MmAllocateContiguousMemory
TICK_FN     equ 0x109760        ; one game tick
TIMEUPD_FN  equ 0xFA920         ; game_time_update(float dt)
WAIT_FN     equ 0xBB060         ; after the frame (vblank bookkeeping)
FP_MATRICES equ 0xA3600         ; node matrices from a pose: stdcall (out, pose, up)

FP_SIZE     equ 0x1EA0          ; +8 weapon, +0x8C the pose: 64 orientations
FP_POSE     equ 0x8C            ; (quaternion, translation, scale: 32 bytes)
POSE_BYTES  equ 64 * 32

; the buffer: the variables, the table, PREV, CUR, the first-person poses
TVAL        equ 0x00            ; t
WRITTEN     equ 0x04            ; CUR holds values (byte)
AMAX        equ 0x08            ; the largest recent tick fraction
BLP         equ 0x0C            ; the pose blended now (0: none)
FPT         equ 0x10            ; each player's tick at its last new pose
FPW         equ 0x20            ; ... its weapon then
FPR         equ 0x30            ; ... poses kept (bytes, up to 2: LAST is valid)
TAB         equ 0x40            ; per object index, 16 bytes: +0 object, +4 salt,
TABSZ       equ 2048 * 16       ; +6 bytes (position + nodes, 0: none), +8 offset
AREA        equ 0x3C000         ; in PREV / CUR, +12 written
PREV        equ TAB + TABSZ
CUR         equ PREV + AREA
FPA         equ CUR + AREA      ; per player: LAST, the last tick's pose (NEW)
SAVE        equ FPA + 8 * POSE_BYTES ; the pose while blended
BUFSZ       equ SAVE + POSE_BYTES
; object: +0x0C position, +0x1A0 node matrices' size (bytes), +0x1A2 their offset

; ---- the call of the tick (0xFAB11)
pre_tick:
    pushad
    call restore                ; (ebp = buf)
    test ebp, ebp
    jnz .have
    push BUFSZ
    call dword [MM_ALLOC]
    test eax, eax
    jz .done                    ; no room yet (the game's loading buffers on a
                                ; fast device): the next tick tries again
    mov edi, eax
    mov ecx, PREV / 4
    push eax
    xor eax, eax
    rep stosd
    pop eax
    mov [buf], eax
    mov ebp, eax
.have:
    cmp ebp, 1
    je .done
    call objects
    xor edx, edx
.loop:
    mov word [edi+6], 0
    mov ax, [esi]
    test ax, ax
    jz .next
    mov [edi+4], ax
    mov ebx, [esi+8]
    mov [edi], ebx
    movzx eax, word [ebx+0x1A0]
    add eax, 12
    push eax
    add eax, edx
    cmp eax, AREA
    pop eax
    ja .next
    mov [edi+6], ax
    mov [edi+8], edx
    pushad
    lea edi, [ebp+PREV+edx]
    lea esi, [ebx+0x0C]
    movsd
    movsd
    movsd
    movsx esi, word [ebx+0x1A2]
    add esi, ebx
    lea ecx, [eax-12]
    shr ecx, 2
    rep movsd
    popad
    add edx, eax
.next:
    add esi, 12
    add edi, 16
    dec ecx
    jnz .loop
.done:
    popad
    jmp TICK_FN

; ---- the call of game_time_update in the main loop (0xBD8A1)
after_ticks:
    push dword [esp+4]
    call TIMEUPD_FN
    pushad
    call interp
    popad
    ret 4

; ---- the call after the frame's render (0xBD983)
after_frame:
    pushad
    call restore
    popad
    jmp WAIT_FN

; ---- the call of FP_MATRICES in the first-person rebuild (0xD7CC0)
fp_pose:
    pushad
    call fp_blend
    popad
    push dword [esp+12]
    push dword [esp+12]
    push dword [esp+12]
    call FP_MATRICES
    pushad
    mov ebp, [buf]              ; the pose back
    cmp ebp, 1
    jbe .ret
    xor edi, edi
    xchg edi, [ebp+BLP]
    test edi, edi
    jz .ret
    lea esi, [ebp+SAVE]
    mov ecx, POSE_BYTES / 4
    rep movsd
.ret:
    popad
    ret 12

; the object headers: ecx of them at esi, the table at edi
objects:
    mov eax, [OBJ_ARRAY]
    movzx ecx, word [eax+0x20]
    mov esi, [eax+0x34]
    lea edi, [ebp+TAB]
    ret

interp:
    call restore                ; (ebp = buf)
    cmp ebp, 1
    jbe .ret
    mov eax, [GAME_TIME]
    fld dword [eax+0x1C]
    fmul dword [eax+0x18]
    fmul dword [c_30]
    fld dword [ebp+AMAX]        ; the frames' tick fractions step by a quarter
    fmul dword [c_decay]        ; tick from an arbitrary phase: show each frame
    fcomi st0, st1              ; the largest fraction (decaying) back, so the
    fcmovb st0, st1             ; steps stay even (t = 1/4 .. 1 for any phase)
    fst dword [ebp+AMAX]
    fsubp st1, st0
    fld1
    faddp st1, st0
    fst dword [ebp+TVAL]
    fld1
    fcompp
    fnstsw ax
    sahf
    jbe .ret                    ; t >= 1: the frame shows the last tick
    call objects
.loop:
    call check
    jnz .next
    lea eax, [ebp+PREV+edx]     ; moved more than 4 units in a tick: no blend
    fld dword [ebx+0x0C]
    fsub dword [eax]
    fmul st0, st0
    fld dword [ebx+0x10]
    fsub dword [eax+4]
    fmul st0, st0
    faddp st1, st0
    fld dword [ebx+0x14]
    fsub dword [eax+8]
    fmul st0, st0
    faddp st1, st0
    fcomp dword [c_jump2]
    fnstsw ax
    sahf
    ja .next
    jp .next
    mov byte [edi+12], 1
    mov byte [ebp+WRITTEN], 1
    pushad
    lea esi, [ebp+PREV+edx]
    lea edi, [ebp+CUR+edx]
    lea edx, [ebx+0x0C]
    push 3
    pop ecx
    call lerp
    movsx edx, word [ebx+0x1A2]
    add edx, ebx
    movzx ecx, word [ebx+0x1A0]
    shr ecx, 2
    jz .none
    call lerp
.none:
    popad
.next:
    add esi, 12
    add edi, 16
    dec ecx
    jnz .loop
.ret:
    ret

; ecx floats at edx: CUR (edi) = them, then PREV (esi) + (them - PREV) * t
lerp:
    mov eax, [edx]
    mov [edi], eax
    fld dword [edx]
    fsub dword [esi]
    fmul dword [ebp+TVAL]
    fadd dword [esi]
    fstp dword [edx]
    add esi, 4
    add edi, 4
    add edx, 4
    dec ecx
    jnz lerp
    ret

; CUR back where interp wrote; ebp = buf on return
restore:
    mov ebp, [buf]
    cmp ebp, 1
    jbe .ret
    cmp byte [ebp+WRITTEN], 0
    je .ret
    mov byte [ebp+WRITTEN], 0
    call objects
.loop:
    cmp byte [edi+12], 0
    je .next
    mov byte [edi+12], 0
    call check
    jnz .next
    pushad
    lea esi, [ebp+CUR+edx]
    lea edi, [ebx+0x0C]
    movsd
    movsd
    movsd
    movsx edi, word [ebx+0x1A2]
    add edi, ebx
    lea ecx, [eax-12]
    shr ecx, 2
    rep movsd
    popad
.next:
    add esi, 12
    add edi, 16
    dec ecx
    jnz .loop
.ret:
    ret

; ZF set when entry edi holds a snapshot of the object in header esi (same
; salt, address and node size): ebx = object, eax = bytes, edx = offset
check:
    movzx eax, word [edi+6]
    test eax, eax
    jz .no
    mov dx, [esi]
    cmp dx, [edi+4]
    jne .ret
    mov ebx, [esi+8]
    cmp ebx, [edi]
    jne .ret
    movzx edx, word [ebx+0x1A0]
    add edx, 12
    cmp edx, eax
    jne .ret
    mov edx, [edi+8]
    cmp eax, eax
.ret:
    ret
.no:
    inc eax
    ret

; the pose fp_pose builds (at [esp+0x2C]): keep each tick's, blend LAST into it
fp_blend:
    mov ebp, [buf]
    cmp ebp, 1
    jbe .ret
    mov esi, [esp+0x2C]
    mov eax, esi                ; the player: (pose - FP_POSE - array) / FP_SIZE
    sub eax, [FP_ARRAY]
    sub eax, FP_POSE
    xor edx, edx
    mov ecx, FP_SIZE
    div ecx
    cmp eax, 3
    ja .ret
    xchg eax, ebx
    mov edx, ebx
    shl edx, 12                 ; 2 poses a player
    lea edx, [ebp+FPA+edx]      ; LAST, then NEW
    mov eax, [GAME_TIME]
    mov eax, [eax+0x0C]
    cmp eax, [ebp+FPT+ebx*4]
    je .blend
    mov [ebp+FPT+ebx*4], eax    ; a new tick: LAST = NEW, NEW = the pose
    mov eax, [esi-FP_POSE+8]
    xchg eax, [ebp+FPW+ebx*4]
    cmp eax, [ebp+FPW+ebx*4]
    je .same
    mov byte [ebp+FPR+ebx], 0   ; another weapon: no blend until two poses
.same:
    cmp byte [ebp+FPR+ebx], 2
    adc byte [ebp+FPR+ebx], 0
    pushad
    mov edi, edx
    lea esi, [edx+POSE_BYTES]
    mov ecx, POSE_BYTES / 4
    rep movsd
    popad
    pushad
    lea edi, [edx+POSE_BYTES]
    mov ecx, POSE_BYTES / 4
    rep movsd
    popad
.blend:
    cmp byte [ebp+FPR+ebx], 2
    jb .ret
    fld dword [ebp+TVAL]
    fld1
    fcompp
    fnstsw ax
    sahf
    jbe .ret                    ; t >= 1
    pushad                      ; SAVE = the pose
    lea edi, [ebp+SAVE]
    mov ecx, POSE_BYTES / 4
    rep movsd
    popad
    mov [ebp+BLP], esi
    mov cl, 64
.node:
    fld dword [edx]             ; the quaternions' dot: LAST's sign flips if < 0
    fmul dword [esi]
    fld dword [edx+4]
    fmul dword [esi+4]
    faddp st1, st0
    fld dword [edx+8]
    fmul dword [esi+8]
    faddp st1, st0
    fld dword [edx+12]
    fmul dword [esi+12]
    faddp st1, st0
    fldz
    fcomip st0, st1
    fstp st0
    seta al
    mov edi, edx                ; the length the blend gets: between theirs
    call qlen                   ; (Halo's quaternions aren't all unit)
    mov edi, esi
    call qlen
    fsub st0, st1
    fmul dword [ebp+TVAL]
    faddp st1, st0
    xor ebx, ebx
.comp:
    fld dword [edx+ebx*4]
    cmp bl, 4
    jae .keep
    test al, al
    jz .keep
    fchs
.keep:
    fld dword [esi+ebx*4]
    fsub st0, st1
    fmul dword [ebp+TVAL]
    faddp st1, st0
    fstp dword [esi+ebx*4]
    inc ebx
    cmp bl, 8
    jb .comp
    mov edi, esi                ; the quaternion scaled to that length
    call qlen
    ftst
    fnstsw ax
    sahf
    je .zero
    fdivp st1, st0
    xor ebx, ebx
.mul:
    fld dword [esi+ebx*4]
    fmul st0, st1
    fstp dword [esi+ebx*4]
    inc ebx
    cmp bl, 4
    jb .mul
    fstp st0
    jmp .step
.zero:
    fstp st0
    fstp st0
.step:
    add esi, 32
    add edx, 32
    dec cl
    jnz .node
.ret:
    ret

; the length of the quaternion at edi, pushed on the FPU stack
qlen:
    fldz
    xor ebx, ebx
.sq:
    fld dword [edi+ebx*4]
    fmul st0, st0
    faddp st1, st0
    inc ebx
    cmp bl, 4
    jb .sq
    fsqrt
    ret

c_30:    dd 30.0
c_decay: dd 0.99
c_jump2: dd 16.0
buf:     dd 0                   ; the buffer (0: not yet)
