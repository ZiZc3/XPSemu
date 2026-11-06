; Halo 2 (4D530064.3) 120 fps: objects drawn between ticks. Lives in the
; header pages, over the logo bitmap (only the dashboard shows it) and the
; zeros after it, up to the PE header copy (0x10CC0..0x118C0).
;
; The 120 fps mod's limiter patches (a frame a vblank, the time step from
; 120 Hz) keep the game at 30 ticks a second; three frames in four run no
; tick. t is how far between the last two ticks a frame is shown: a quarter
; tick apart, 1/4, 1/2, 3/4 and 1 (the last tick).
; - Before each tick (the tick loop 0x137F70 calls one at 0x137F89) the
;   objects' positions and node matrices are copied to PREV.
; - After the frame's ticks (0x12B632) the current values go to CUR and the
;   objects get PREV + (CUR - PREV) * t for the frame's camera, director and
;   render; at the next frame's limiter (0x12B58F) CUR goes back.
BITS 32
ORG 0x10CC0

OBJ_ARRAY   equ 0x4E0300        ; the object header data array (data_array *)
GAME_TIME   equ 0x510C54        ; game time globals *: +0x10 the tick fraction left
MM_ALLOC    equ 0x411524        ; kernel thunk: MmAllocateContiguousMemory
TICK_FN     equ 0x137FE0        ; one game tick
TICKS_FN    equ 0x137F70        ; the frame's ticks (eax: how many; ebx used)
LIMIT_FN    equ 0x12B0E0        ; the frame limiter (returns the frame's dt)
POS         equ 0x64            ; object: position (12 bytes)
NODES       equ 0x114           ; object: node matrices' size (word), offset (word)

; the buffer: the variables, the table, PREV, CUR
TVAL        equ 0x00            ; t
WRITTEN     equ 0x04            ; CUR holds values (byte)
AMAX        equ 0x08            ; the largest recent tick fraction
USED        equ 0x10            ; the last tick's snapshot: bytes, objects, objects left out
NSEEN       equ 0x14            ; (for debugging: NC_PEEK=*11800:32)
NSKIP       equ 0x18
PASSF       equ 0x1C            ; the snapshot's pass: 2 characters, vehicles, creatures; 1 the
                                ; rest that moves
OFFV        equ 0x20            ; nonzero: no blending (an A/B switch, poked; the code isn't
                                ; patched at runtime: the JIT keeps its translation)
TAB         equ 0x40            ; per object index, 16 bytes: +0 object, +4 salt,
NTAB        equ 2048            ; +6 bytes (position + nodes, 0: none), +8 offset
TABSZ       equ NTAB * 16       ; in PREV / CUR, +12 written
AREA        equ 0x30000         ; Cairo Station: 45 bipeds' nodes are 120 KB of ~178 KB
PREV        equ TAB + TABSZ
CUR         equ PREV + AREA
BUFSZ       equ CUR + AREA

; ---- the tick loop's call of a tick (0x137F89)
pre_tick:
    pushad
    call restore                ; (ebp = buf)
    test ebp, ebp
    jnz .have
    push BUFSZ
    call dword [MM_ALLOC]
    test eax, eax
    jz .done                    ; no room yet: the next tick tries again
    mov edi, eax
    mov ecx, PREV / 4
    push eax
    xor eax, eax
    rep stosd
    pop eax
    mov [buf], eax
    mov ebp, eax
.have:
    call objects
    mov [ebp+NSEEN], ecx
    and dword [ebp+NSKIP], 0
    and dword [ebp+USED], 0
    jecxz .done
    push ecx
    push edi
.clear:
    mov word [edi+6], 0
    add edi, 16
    dec ecx
    jnz .clear
    pop edi
    pop ecx
    ; Only the types that move, and the characters first: in the array's
    ; order the level's scenery filled the area and the characters spawned
    ; after it were left out (Cairo Station: 137 of 736 objects).
    push 2
    pop eax
    call snap_pass
    push 1
    pop eax
    call snap_pass
.done:
    popad
    jmp TICK_FN

; one pass over the ecx headers at esi (the table at edi), from [ebp+USED]
; on: eax = 2 the objects of the types in c_first, 1 the other types in
; c_movers
snap_pass:
    pushad
    mov [ebp+PASSF], eax
    mov edx, [ebp+USED]
.loop:
    mov ax, [esi]
    test ax, ax
    jz .next
    mov ebx, [esi+8]
    test ebx, ebx
    jz .next
    movzx eax, byte [esi+3]     ; the object's type
    push edx
    xor edx, edx
    cmp eax, 31
    ja .typed
    bt dword [c_movers], eax
    adc edx, 0
    bt dword [c_first], eax
    adc edx, 0
.typed:
    cmp edx, [ebp+PASSF]
    pop edx
    jne .next
    mov ax, [esi]
    mov [edi+4], ax
    mov [edi], ebx
    call nodes_size             ; eax = 12 + the node bytes
    push eax
    add eax, edx
    cmp eax, AREA
    pop eax
    ja .full
    mov [edi+6], ax
    mov [edi+8], edx
    pushad
    lea edi, [ebp+PREV+edx]
    lea esi, [ebx+POS]
    movsd
    movsd
    movsd
    lea ecx, [eax-12]
    shr ecx, 2
    jz .nonodes
    movzx esi, word [ebx+NODES+2]
    add esi, ebx
    rep movsd
.nonodes:
    popad
    add edx, eax
    jmp .next
.full:
    inc dword [ebp+NSKIP]
.next:
    add esi, 12
    add edi, 16
    dec ecx
    jnz .loop
    mov [ebp+USED], edx
    popad
    ret

; ---- the call of the frame's ticks (0x12B632)
after_ticks:
    call TICKS_FN
    pushad
    call interp
    popad
    ret

; ---- the call of the frame limiter (0x12B58F): the last frame drawn, CUR back
before_limit:
    pushad
    call restore
    popad
    jmp LIMIT_FN

; eax = 12 + the node matrices' bytes of the object at ebx (none: 12)
nodes_size:
    movzx eax, word [ebx+NODES+2]
    cmp ax, 0xFFFF
    je .none
    movzx eax, word [ebx+NODES]
    and eax, ~3
    add eax, 12
    ret
.none:
    mov eax, 12
    ret

; the object headers: ecx of them at esi, the table at edi
objects:
    mov eax, [OBJ_ARRAY]
    xor ecx, ecx
    test eax, eax
    jz .ret
    mov ecx, [eax+0x38]         ; the first unallocated index
    cmp ecx, NTAB
    jbe .n
    mov ecx, NTAB
.n:
    mov esi, [eax+0x44]
    lea edi, [ebp+TAB]
.ret:
    ret

interp:
    call restore                ; (ebp = buf)
    test ebp, ebp
    jz .ret
    cmp byte [ebp+OFFV], 0
    jne .ret
    mov eax, [GAME_TIME]
    test eax, eax
    jz .ret
    fld dword [eax+0x10]        ; the frame's tick fraction
    fld dword [ebp+AMAX]        ; the frames' fractions step by a quarter tick
    fmul dword [c_decay]        ; from an arbitrary phase: show each frame the
    fcomi st0, st1              ; largest fraction (decaying) back, so the steps
    fcmovb st0, st1             ; stay even (t = 1/4 .. 1 for any phase)
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
    jecxz .ret
.loop:
    call check
    jnz .next
    lea eax, [ebp+PREV+edx]     ; moved more than 4 units in a tick: no blend
    fld dword [ebx+POS]
    fsub dword [eax]
    fmul st0, st0
    fld dword [ebx+POS+4]
    fsub dword [eax+4]
    fmul st0, st0
    faddp st1, st0
    fld dword [ebx+POS+8]
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
    movzx eax, word [edi+6]
    lea esi, [ebp+PREV+edx]
    lea edi, [ebp+CUR+edx]
    lea edx, [ebx+POS]
    push 3
    pop ecx
    call lerp
    lea ecx, [eax-12]
    shr ecx, 2
    jz .none
    movzx edx, word [ebx+NODES+2]
    add edx, ebx
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
    push eax
.l:
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
    jnz .l
    pop eax
    ret

; CUR back where interp wrote; ebp = buf on return
restore:
    mov ebp, [buf]
    test ebp, ebp
    jz .ret
    cmp byte [ebp+WRITTEN], 0
    je .ret
    mov byte [ebp+WRITTEN], 0
    call objects
    test ecx, ecx
    jz .ret
.loop:
    cmp byte [edi+12], 0
    je .next
    mov byte [edi+12], 0
    call check
    jnz .next
    pushad
    lea esi, [ebp+CUR+edx]
    lea edi, [ebx+POS]
    movsd
    movsd
    movsd
    lea ecx, [eax-12]
    shr ecx, 2
    jz .none
    movzx edi, word [ebx+NODES+2]
    add edi, ebx
    rep movsd
.none:
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
    push eax
    call nodes_size
    mov edx, eax
    pop eax
    cmp edx, eax
    jne .ret
    mov edx, [edi+8]
    cmp eax, eax
.ret:
    ret
.no:
    inc eax
    ret

; the object types that move: biped, vehicle, weapon, equipment, garbage,
; projectile, machine (doors, lifts), creature; not scenery, crates (Cairo:
; 193 of them, 12 KB), controls, lights or sound scenery. Bipeds, vehicles
; and creatures first.
c_movers: dd (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 12)
c_first: dd (1 << 0) | (1 << 1) | (1 << 12)
c_decay: dd 0.99
c_jump2: dd 16.0
code_end:
buf         equ 0x11800         ; the buffer (0: not yet), in the zeros past the logo
