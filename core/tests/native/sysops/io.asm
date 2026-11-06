%ifdef CONFIG
{
  "RegData": {
    "RBX": "0xCAFEBABE",
    "RCX": "0x5A",
    "RSI": "0xFFFF1357"
  },
  "Mode": "32BIT"
}
%endif

; fexrun --sysops: ports hold what was last written to them (OUT stores the
; operand-size bytes; IN merges them into AL/AX/EAX).
mov edx, 0x1234
mov eax, 0xCAFEBABE
out dx, eax
xor eax, eax
in eax, dx
mov ebx, eax

mov al, 0x5A
out 0x80, al
mov eax, 0
in al, 0x80
mov ecx, eax

mov ax, 0x1357
out dx, ax
mov eax, 0xFFFFFFFF
in ax, dx
mov esi, eax

hlt
