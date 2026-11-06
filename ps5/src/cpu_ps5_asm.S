// XOG: the doors of the native CPU. Our own code. GPL-3.0-or-later.
//
// struct NcFrame (cpu_ps5.c): regs[8] at 0, rip 0x40, eflags 0x48, then fx at 0x60.
#define F_RIP 0x40
#define F_EFLAGS 0x48
#define F_FX 0x60

.text

// void rx_enter(void): into the guest from rx_frame; comes back through rx_leave.
.globl rx_enter
rx_enter:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, rx_host_rsp(%rip)
    movw rx_data_sel(%rip), %ax
    movw %ax, %ds
    movw %ax, %es
    jmp rx_resume

// Off the guest: nc_cpu_run returns. Reached by a signal's return with the host's cs
// and ss in its context, or from the gate.
.globl rx_leave
rx_leave:
    movq rx_host_rsp(%rip), %rsp
    cld
    fninit
    ldmxcsr rx_host_mxcsr(%rip)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

// A guest's far call lands here, through the kernel area's 64-bit pad: RSP is the
// guest's stack with its return CS:EIP on top, and its upper half is undefined.
.globl rx_gate_entry
rx_gate_entry:
    movl %esp, %esp
    leaq rx_frame(%rip), %r11
    movl %eax, 0x00(%r11)
    movl %ecx, 0x08(%r11)
    movl %edx, 0x10(%r11)
    movl %ebx, 0x18(%r11)
    movl %ebp, 0x28(%r11)
    movl %esi, 0x30(%r11)
    movl %edi, 0x38(%r11)
    movl (%rsp), %eax           // the return EIP
    movq %rax, F_RIP(%r11)
    leal 8(%rsp), %eax          // ESP without the far return
    movl %eax, 0x20(%r11)
    pushfq
    popq %rax
    movl %eax, F_EFLAGS(%r11)
    fxsave64 F_FX(%r11)
    movq rx_gate_stack(%rip), %rsp
    cld
    ldmxcsr rx_host_mxcsr(%rip)
    fninit
    call rx_gate_c
    jmp rx_gate_decide

// The way back. A kick that lands from here to rx_resume_end restarts at rx_gate_exit
// (cpu_ps5.c), so an interrupt asked for after the look below is never left waiting.
.globl rx_gate_exit
rx_gate_exit:
    movq rx_gate_stack(%rip), %rsp
    cmpl $0, rx_irq_pending(%rip)
    je rx_resume
    call rx_gate_again
rx_gate_decide:
    cmpl $1, %eax
    je rx_leave
    cmpl $2, %eax
    jne rx_gate_exit
.globl rx_resume
rx_resume:
    leaq rx_frame(%rip), %r15
    fxrstor64 F_FX(%r15)
    movzwq rx_data_sel(%rip), %rax
    pushq %rax                  // ss
    movl 0x20(%r15), %eax       // esp: NC_ESP is 4
    pushq %rax
    movl F_EFLAGS(%r15), %eax
    andl $0x00040CD5, %eax
    orl $0x202, %eax
    orl rx_entry_tf(%rip), %eax
    pushq %rax                  // rflags: IF stays on, the guest's is virtual
    movl $0, rx_entry_tf(%rip)
    movzwq rx_guest_cs(%rip), %rax
    pushq %rax
    movl F_RIP(%r15), %eax
    pushq %rax
    movl 0x00(%r15), %eax
    movl 0x08(%r15), %ecx
    movl 0x10(%r15), %edx
    movl 0x18(%r15), %ebx
    movl 0x28(%r15), %ebp
    movl 0x30(%r15), %esi
    movl 0x38(%r15), %edi
    iretq
.globl rx_resume_end
rx_resume_end:
