; Crimson Skies (4D530021.1) 60 fps: the header page past the headers
; (sizeof_headers 0xF1C -> 0x1000).
BITS 32
ORG 0x10F20

FTOL        equ 0x1F1A28        ; _ftol2: st0 -> eax (truncated), pops st0
C_1000      equ 0x28EE18        ; 1000.0f

; the frame gate's minimum period (0x1308D9, 0x130B23 load it)
c_period:   dd 0x3C888889       ; 1/60

; ---- the gate's ms conversion (0x12FBCD, call FTOL): esi = the timer
; (+8 the counter's frequency, +0x10 the time base, just set to now),
; st0 = the frame's time in ms. The game advances by whole ms: the fraction
; it drops (0.67 ms of 16.67 at 60 fps) goes back into the time base, so it
; counts in the next frame.
ms_carry:
    fld st0
    call FTOL                   ; eax = whole ms
    push eax
    push eax
    fisub dword [esp]           ; the fraction, ms
    fild qword [esi+8]
    fmulp st1, st0
    fdiv dword [C_1000]         ; in counter ticks
    fistp dword [esp]
    pop ecx
    sub [esi+0x10], ecx
    sbb dword [esi+0x14], 0
    pop eax
    ret
