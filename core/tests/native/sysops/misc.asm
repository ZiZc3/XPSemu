%ifdef CONFIG
{
  "RegData": {
    "RBX": "0x0",
    "RCX": "0x200",
    "RDX": "0x1021",
    "RSI": "0x200B",
    "RDI": "0x8001003B",
    "RBP": "0xC0DE"
  },
  "Mode": "32BIT"
}
%endif

; fexrun --sysops: CLI/STI drive EFLAGS.IF; INT n sets EAX = 0x1000 + n; traps
; set EAX = 0x2000 + SystemOpKind (INT3 = 11); CR0 reads 0x8001003B; WBINVD
; and INVLPG are no-ops that must resume after themselves.
mov esp, 0xC0001000
cli
pushfd
pop eax
and eax, 0x200
mov ebx, eax

sti
pushfd
pop eax
and eax, 0x200
mov ecx, eax

int 0x21
mov edx, eax

int3
mov esi, eax

mov eax, cr0
mov edi, eax

wbinvd
invlpg [0x10000]
mov ebp, 0xC0DE

hlt
