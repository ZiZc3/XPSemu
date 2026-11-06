%ifdef CONFIG
{
  "RegData": {
    "RAX": "0xC0DE",
    "RDX": "0x1"
  },
  "Mode": "32BIT"
}
%endif

; fexrun --sysops: writing port 0xF0 makes the harness request an interrupt
; 20 ms later; its handler writes 0xC0DE to [0xC0000800]. This loop never
; leaves guest code on its own, so it only sees the value if the request is
; taken at the start of its block. EDX = 1: the flag ended the loop, not the
; countdown.
mov esp, 0xC0001000
mov dword [0xC0000800], 0
out 0xF0, al
mov ecx, 0x7FFFFFFF
.spin:
cmp dword [0xC0000800], 0
jne .done
dec ecx
jnz .spin
.done:
mov eax, [0xC0000800]
xor edx, edx
test ecx, ecx
setnz dl
xor ecx, ecx ; the countdown and its flags depend on timing (--diff compares runs)
hlt
