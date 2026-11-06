%ifdef CONFIG
{
  "RegData": {
    "RAX": "0x1"
  },
  "Mode": "32BIT"
}
%endif

; fexrun --sysops: RDTSC is the host counter scaled to 733.333 MHz. INT 0x42
; makes the harness compare EDI:ESI with its own scaled counter read and set
; EAX = 1 when the guest value is not ahead of it and less than 1 ms behind.
; ESI/EDI are cleared afterwards so --diff runs compare equal.
rdtsc
mov esi, eax
mov edi, edx
int 0x42
xor esi, esi
xor edi, edi

hlt
