.section .text

.macro EXCEPTION_NO_ERROR vector
.global exception_stub_\vector
.type exception_stub_\vector, @function
exception_stub_\vector:
    pushq $0
    pushq $\vector
    jmp exception_common
.endm

.macro EXCEPTION_ERROR_CODE vector
.global exception_stub_\vector
.type exception_stub_\vector, @function
exception_stub_\vector:
    pushq $\vector
    jmp exception_common
.endm

EXCEPTION_NO_ERROR 0
EXCEPTION_NO_ERROR 1
EXCEPTION_NO_ERROR 2
EXCEPTION_NO_ERROR 3
EXCEPTION_NO_ERROR 4
EXCEPTION_NO_ERROR 5
EXCEPTION_NO_ERROR 6
EXCEPTION_NO_ERROR 7
EXCEPTION_ERROR_CODE 8
EXCEPTION_NO_ERROR 9
EXCEPTION_ERROR_CODE 10
EXCEPTION_ERROR_CODE 11
EXCEPTION_ERROR_CODE 12
EXCEPTION_ERROR_CODE 13
EXCEPTION_ERROR_CODE 14
EXCEPTION_NO_ERROR 15
EXCEPTION_NO_ERROR 16
EXCEPTION_ERROR_CODE 17
EXCEPTION_NO_ERROR 18
EXCEPTION_NO_ERROR 19
EXCEPTION_ERROR_CODE 20
EXCEPTION_ERROR_CODE 21
EXCEPTION_NO_ERROR 22
EXCEPTION_NO_ERROR 23
EXCEPTION_NO_ERROR 24
EXCEPTION_NO_ERROR 25
EXCEPTION_NO_ERROR 26
EXCEPTION_NO_ERROR 27
EXCEPTION_NO_ERROR 28
EXCEPTION_ERROR_CODE 29
EXCEPTION_ERROR_CODE 30
EXCEPTION_NO_ERROR 31

.macro IRQ_STUB irq
.global irq_stub_\irq
.type irq_stub_\irq, @function
irq_stub_\irq:
    pushq $0
    pushq $(32 + \irq)
    jmp irq_common
.endm

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

.global syscall_stub
.type syscall_stub, @function
syscall_stub:
    pushq $0
    pushq $128
    jmp syscall_common

.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    swapgs
    movq %rsp, %gs:0
    movq %gs:8, %rsp
    subq $8, %rsp
    pushq %r11
    pushq %rcx
    pushq %rax
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r10
    pushq %r8
    pushq %r9
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movl $0xc0000100, %ecx
    rdmsr
    shlq $32, %rdx
    orq %rdx, %rax
    movq %rax, 120(%rsp)
    xorl %eax, %eax
    xorl %edx, %edx
    wrmsr

    movq %rsp, %r12
    andq $-16, %rsp
    movq 120(%r12), %rdi
    call scheduler_linux_syscall_enter
    movq %r12, %rdi
    call user_linux_syscall_dispatch
    movq %rax, %rbx
    testq %rbx, %rbx
    jnz 1f
    movq %r12, %rdi
    call user_linux_signal_deliver
    movq %rax, %rbx
1:
    movq 120(%r12), %rdi
    call scheduler_linux_syscall_leave
    movq %rbx, %rax
    movq %r12, %rsp
    testq %rax, %rax
    jnz linux_syscall_exit

    movq 120(%r12), %rax
    movq %rax, %rdx
    shrq $32, %rdx
    movl $0xc0000100, %ecx
    wrmsr

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax
    popq %rcx
    popq %r11
    movq %gs:0, %rsp
    swapgs
    sysretq

linux_syscall_exit:
    movq 96(%r12), %rax
    swapgs
    sti
    jmp user_exit_trampoline

.type syscall_common, @function
syscall_common:
    cld
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %r12
    movq %r12, %rdi
    andq $-16, %rsp
    call user_syscall_dispatch
    movq %r12, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    addq $16, %rsp
    iretq

.type irq_common, @function
irq_common:
    cld
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, %r12
    movq 120(%r12), %rdi
    movq %r12, %rsi
    andq $-16, %rsp
    call irq_handler
    movq %rax, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    addq $16, %rsp
    iretq

.type exception_common, @function
exception_common:
    cld
    movq %rsp, %r12
    movq 0(%r12), %rdi
    movq 8(%r12), %rsi
    movq 16(%r12), %rdx
    movq 24(%r12), %rcx
    movq %r12, %r8
    andq $-16, %rsp
    call exception_handler

    movq %r12, %rsp
    addq $16, %rsp
    iretq

.section .note.GNU-stack,"",@progbits
