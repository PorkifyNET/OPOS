.section .text
.global _start
.type _start, @function
_start:
    testq $15, %rsp
    jne 1f
    movq 0(%rsp), %rbp
    cmpq $1, %rbp
    je 2f
    cmpq $2, %rbp
    jne 1f
2:
    movq 8(%rsp), %rsi
    leaq expected_path(%rip), %r12
    movq %r12, %rdi
    movq $18, %r10
    movq $18, %rcx
    repe cmpsb
    je .Ldynamic_stack
    movq 8(%rsp), %rsi
    leaq expected_pie_path(%rip), %r12
    movq %r12, %rdi
    movq $14, %r10
    movq $14, %rcx
    repe cmpsb
    jne 1f
    leaq expected_pie_argument(%rip), %rbx
    movq $4, %rcx
    movq $0x04000040, %r14
    movq $7, %r15
    movq $0x04001000, %r13
    jmp .Lstack_common
.Ldynamic_stack:
    leaq expected_argument(%rip), %rbx
    movq $4, %rcx
    movq $0x02000040, %r14
    movq $7, %r15
    movq $0x02001000, %r13
.Lstack_common:
    cmpq $1, %rbp
    je 2f
    movq 16(%rsp), %rsi
    movq %rbx, %rdi
    repe cmpsb
    jne 1f
2:
    leaq 16(%rsp,%rbp,8), %rbp
    cmpq $0, -8(%rbp)
    jne 1f
    cmpq $0, 0(%rbp)
    jne 1f
    cmpq $3, 8(%rbp)
    jne 1f
    cmpq %r14, 16(%rbp)
    jne 1f
    cmpq $4, 24(%rbp)
    jne 1f
    cmpq $56, 32(%rbp)
    jne 1f
    cmpq $5, 40(%rbp)
    jne 1f
    cmpq %r15, 48(%rbp)
    jne 1f
    cmpq $6, 56(%rbp)
    jne 1f
    cmpq $4096, 64(%rbp)
    jne 1f
    cmpq $7, 72(%rbp)
    jne 1f
    cmpq $0x10000000, 80(%rbp)
    jne 1f
    cmpq $9, 88(%rbp)
    jne 1f
    cmpq %r13, 96(%rbp)
    jne 1f
    cmpq $11, 104(%rbp)
    jne 1f
    cmpq $0, 112(%rbp)
    jne 1f
    cmpq $12, 120(%rbp)
    jne 1f
    cmpq $0, 128(%rbp)
    jne 1f
    cmpq $13, 136(%rbp)
    jne 1f
    cmpq $0, 144(%rbp)
    jne 1f
    cmpq $14, 152(%rbp)
    jne 1f
    cmpq $0, 160(%rbp)
    jne 1f
    cmpq $23, 168(%rbp)
    jne 1f
    cmpq $0, 176(%rbp)
    jne 1f
    cmpq $25, 184(%rbp)
    jne 1f
    movq 192(%rbp), %rax
    testq $15, %rax
    jne 1f
    cmpq $0x7ffff000, %rax
    jb 1f
    cmpq $0x7ffffff0, %rax
    ja 1f
    movq 0(%rax), %rdx
    orq 8(%rax), %rdx
    je 1f
    cmpq $31, 200(%rbp)
    jne 1f
    movq 208(%rbp), %rsi
    movq %r12, %rdi
    movq %r10, %rcx
    repe cmpsb
    jne 1f
    cmpq $15, 216(%rbp)
    jne 1f
    movq 224(%rbp), %rax
    cmpq $0x7ffff000, %rax
    jb 1f
    cmpq $0x7ffffff9, %rax
    ja 1f
    cmpl $0x5f363878, 0(%rax)
    jne 1f
    cmpw $0x3436, 4(%rax)
    jne 1f
    cmpb $0, 6(%rax)
    jne 1f
    cmpq $16, 232(%rbp)
    jne 1f
    movl 240(%rbp), %eax
    andl $0x07808101, %eax
    cmpl $0x07808101, %eax
    jne 1f
    cmpq $26, 248(%rbp)
    jne 1f
    cmpq $0, 256(%rbp)
    jne 1f
    cmpq $17, 264(%rbp)
    jne 1f
    cmpq $100, 272(%rbp)
    jne 1f
    cmpq $8, 280(%rbp)
    jne 1f
    cmpq $0, 288(%rbp)
    jne 1f
    cmpq $0, 296(%rbp)
    jne 1f
    cmpq $0, 304(%rbp)
    jne 1f
    cmpq $41, initialized_value(%rip)
    jne 1f
    cmpq $0, zero_value(%rip)
    jne 1f
    incq initialized_value(%rip)
    movq $0x4f504f53, zero_value(%rip)
    cmpq $42, initialized_value(%rip)
    jne 1f
    cmpq $0x4f504f53, zero_value(%rip)
    jne 1f
    movq %rsp, %rdi
    call opos_dynamic_link
    testq %rax, %rax
    jne 2f
    leaq opos_dynamic_finalize(%rip), %rdx
    jmp *96(%rbp)
2:
    movq %rax, %rdi
    movq $231, %rax
    syscall
    ud2
1:
    movq $127, %rdi
    movq $231, %rax
    syscall
    ud2

.global opos_tlsdesc_return
.type opos_tlsdesc_return, @function
opos_tlsdesc_return:
    movq 8(%rax), %rax
    ret

.global opos_tlsdesc_undefweak
.type opos_tlsdesc_undefweak, @function
opos_tlsdesc_undefweak:
    movq 8(%rax), %rax
    subq %fs:0, %rax
    ret

.section .rodata
expected_path:
    .asciz "/bin/dynamic-test"
expected_argument:
    .asciz "dyn"
expected_pie_path:
    .asciz "/bin/pie-test"
expected_pie_argument:
    .asciz "pie"

.section .data
.balign 8
initialized_value:
    .quad 41

.section .bss
.balign 8
zero_value:
    .quad 0

.section .note.GNU-stack,"",@progbits
