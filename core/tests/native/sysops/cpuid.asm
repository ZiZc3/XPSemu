%ifdef CONFIG
{
  "RegData": {
    "RSI": "0x68A",
    "RDI": "0x0383F9FF",
    "RBX": "0x756E6547",
    "RDX": "0x49656E69",
    "RCX": "0x6C65746E",
    "RBP": "0x2"
  },
  "Mode": "32BIT"
}
%endif

; fexrun --sysops: CPUID describes the Xbox's Pentium III (family 6, model 8,
; stepping 10; MMX + SSE, no SSE2), vendor GenuineIntel, max leaf 2.
mov eax, 1
cpuid
mov esi, eax
mov edi, edx

mov eax, 0
cpuid
mov ebp, eax

hlt
