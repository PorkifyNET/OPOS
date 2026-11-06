.section .text
.global _start
.type _start, @function
_start:
    testq $15, %rsp
    jne 1f
    cmpq $2, 0(%rsp)
    jne 1f
    movq 8(%rsp), %rsi
    leaq expected_path(%rip), %rdi
    movq $15, %rcx
    repe cmpsb
    jne 1f
    movq 16(%rsp), %rsi
    leaq expected_argument(%rip), %rdi
    movq $3, %rcx
    repe cmpsb
    jne 1f
    cmpq $0, 24(%rsp)
    jne 1f
    cmpq $0, 32(%rsp)
    jne 1f

    movq $5, %rax
    movq $3, %rdi
    leaq stat_buffer(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq $5, %rax
    movq $4, %rdi
    leaq stat_buffer(%rip), %rsi
    syscall
    cmpq $-9, %rax
    jne 1f

    movq $158, %rax
    movq $0x1003, %rdi
    leaq fs_base_value(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    cmpq $0, fs_base_value(%rip)
    jne 1f

    movq $9, %rax
    xorq %rdi, %rdi
    movq $4096, %rsi
    movq $3, %rdx
    movq $0x22, %r10
    movq $-1, %r8
    xorq %r9, %r9
    syscall
    cmpq $0x10000000, %rax
    jne 1f
    movq $0x1122, (%rax)
    movq $10, data_sentinel(%rip)
    movq $30, -16(%rsp)

    movq $158, %rax
    movq $0x1002, %rdi
    leaq data_sentinel(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f

    movq $57, %rax
    syscall
    testq %rax, %rax
    js 1f
    jz 2f

    movq %rax, %r12
    movq $158, %rax
    movq $0x1003, %rdi
    leaq fs_base_value(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    leaq data_sentinel(%rip), %rax
    cmpq %rax, fs_base_value(%rip)
    jne 1f
    movq $61, %rax
    movq %r12, %rdi
    leaq -8(%rsp), %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    syscall
    cmpq %r12, %rax
    jne 1f
    cmpl $(42 << 8), -8(%rsp)
    jne 1f
    cmpq $10, data_sentinel(%rip)
    jne 1f
    cmpq $30, -16(%rsp)
    jne 1f
    cmpq $0x1122, 0x10000000
    jne 1f
    movq $158, %rax
    movq $0x1003, %rdi
    leaq fs_base_value(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    leaq data_sentinel(%rip), %rax
    cmpq %rax, fs_base_value(%rip)
    jne 1f
    movq $8, %rax
    movq $3, %rdi
    xorq %rsi, %rsi
    movq $1, %rdx
    syscall
    cmpq $1, %rax
    jne 1f
    movq $61, %rax
    movq %r12, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    syscall
    cmpq $-10, %rax
    jne 1f
    movq $11, %rax
    movq $0x10000000, %rdi
    movq $4096, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq $158, %rax
    movq $0x1002, %rdi
    xorq %rsi, %rsi
    syscall
    testq %rax, %rax
    jne 1f

    movq $2, %rax
    leaq exec_test_path(%rip), %rdi
    xorq %rsi, %rsi
    syscall
    cmpq $4, %rax
    jne 1f
    movq $9, %rax
    xorq %rdi, %rdi
    movq $8192, %rsi
    movq $1, %rdx
    movq $2, %r10
    movq $4, %r8
    xorq %r9, %r9
    syscall
    cmpq $0x10000000, %rax
    jne 1f
    movq %rax, %r15
    cmpl $0x464c457f, (%r15)
    jne 1f
    movq $11, %rax
    movq %r15, %rdi
    movq $8192, %rsi
    syscall
    testq %rax, %rax
    jne 1f

    movq $9, %rax
    xorq %rdi, %rdi
    movq $4096, %rsi
    movq $3, %rdx
    movq $2, %r10
    movq $4, %r8
    xorq %r9, %r9
    syscall
    cmpq $0x10000000, %rax
    jne 1f
    movq %rax, %r15
    movb $0, (%r15)
    movq $8, %rax
    movq $4, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    xorq %rax, %rax
    movq $4, %rdi
    leaq file_map_buffer(%rip), %rsi
    movq $4, %rdx
    syscall
    cmpq $4, %rax
    jne 1f
    cmpl $0x464c457f, file_map_buffer(%rip)
    jne 1f
    cmpb $0, (%r15)
    jne 1f
    movq $11, %rax
    movq %r15, %rdi
    movq $4096, %rsi
    syscall
    testq %rax, %rax
    jne 1f

    movq $9, %rax
    xorq %rdi, %rdi
    movq $4096, %rsi
    movq $5, %rdx
    movq $2, %r10
    movq $4, %r8
    movq $4096, %r9
    syscall
    cmpq $0x10000000, %rax
    jne 1f
    movq %rax, %r15
    movq $11, %rax
    movq %r15, %rdi
    movq $4096, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq $3, %rax
    movq $4, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $293, %rax
    leaq pipe_descriptors(%rip), %rdi
    xorq %rsi, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movl pipe_descriptors(%rip), %r13d
    movl pipe_descriptors+4(%rip), %r14d
    cmpq $3, %r13
    jb 1f
    cmpq $3, %r14
    jb 1f
    cmpq %r13, %r14
    je 1f
    movq $5, %rax
    movq %r13, %rdi
    leaq stat_buffer(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movl stat_buffer+24(%rip), %eax
    andl $0170000, %eax
    cmpl $0010000, %eax
    jne 1f
    movq $8, %rax
    movq %r13, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    syscall
    cmpq $-29, %rax
    jne 1f

    movq $57, %rax
    syscall
    testq %rax, %rax
    js 1f
    jz 3f
    movq %rax, %r12
    movq $3, %rax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $0, pipe_buffer(%rip)
    xorq %rax, %rax
    movq %r13, %rdi
    leaq pipe_buffer(%rip), %rsi
    movq $32, %rdx
    syscall
    cmpq $7, %rax
    jne 1f
    cmpl $0x65706970, pipe_buffer(%rip)
    jne 1f
    cmpw $0x6b6f, pipe_buffer+5(%rip)
    jne 1f
    cmpb $'-', pipe_buffer+4(%rip)
    jne 1f
    xorq %rax, %rax
    movq %r13, %rdi
    leaq pipe_buffer(%rip), %rsi
    movq $1, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    movq $61, %rax
    movq %r12, %rdi
    leaq -8(%rsp), %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    syscall
    cmpq %r12, %rax
    jne 1f
    cmpl $(7 << 8), -8(%rsp)
    jne 1f
    movq $3, %rax
    movq %r13, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $22, %rax
    leaq pipe_descriptors(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movl pipe_descriptors(%rip), %r13d
    movl pipe_descriptors+4(%rip), %r14d
    movq $3, %rax
    movq %r13, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movb $'x', pipe_buffer(%rip)
    movq $1, %rax
    movq %r14, %rdi
    leaq pipe_buffer(%rip), %rsi
    movq $1, %rdx
    syscall
    cmpq $-32, %rax
    jne 1f
    movq $3, %rax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $9, %rax
    xorq %rdi, %rdi
    movq $4096, %rsi
    movq $3, %rdx
    movq $0x22, %r10
    movq $-1, %r8
    xorq %r9, %r9
    syscall
    cmpq $0x10000000, %rax
    jne 1f
    movq $22, %rax
    leaq pipe_descriptors(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movl pipe_descriptors(%rip), %r13d
    movl pipe_descriptors+4(%rip), %r14d
    movq $1, %rax
    movq %r14, %rdi
    movq $0x10000000, %rsi
    movq $4096, %rdx
    syscall
    cmpq $4096, %rax
    jne 1f
    movb $'z', pipe_buffer(%rip)
    movq $57, %rax
    syscall
    testq %rax, %rax
    js 1f
    jz 4f
    movq %rax, %r12
    movq $3, %rax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $0, -96(%rsp)
    movq $20000000, -88(%rsp)
    movq $35, %rax
    leaq -96(%rsp), %rdi
    xorq %rsi, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    xorq %rax, %rax
    movq %r13, %rdi
    leaq pipe_buffer(%rip), %rsi
    movq $1, %rdx
    syscall
    cmpq $1, %rax
    jne 1f
    movq $61, %rax
    movq %r12, %rdi
    leaq -8(%rsp), %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    syscall
    cmpq %r12, %rax
    jne 1f
    cmpl $(9 << 8), -8(%rsp)
    jne 1f
    movq $3, %rax
    movq %r13, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $11, %rax
    movq $0x10000000, %rdi
    movq $4096, %rsi
    syscall
    testq %rax, %rax
    jne 1f

    movq $3, %rax
    movq $3, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    leaq dynamic_test_path(%rip), %rax
    movq %rax, -32(%rsp)
    leaq dynamic_test_argument(%rip), %rax
    movq %rax, -24(%rsp)
    movq $0, -16(%rsp)
    movq $0, -8(%rsp)
    movq $59, %rax
    leaq dynamic_test_path(%rip), %rdi
    leaq -32(%rsp), %rsi
    leaq -8(%rsp), %rdx
    syscall
    negq %rax
    movq %rax, %rdi
    movq $231, %rax
    syscall
2:
    movq $158, %rax
    movq $0x1003, %rdi
    leaq fs_base_value(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    leaq data_sentinel(%rip), %rax
    cmpq %rax, fs_base_value(%rip)
    jne 1f
    cmpq $10, data_sentinel(%rip)
    jne 1f
    cmpq $30, -16(%rsp)
    jne 1f
    cmpq $0x1122, 0x10000000
    jne 1f
    movq $20, data_sentinel(%rip)
    movq $40, -16(%rsp)
    movq $0x3344, 0x10000000
    movq $8, %rax
    movq $3, %rdi
    movq $1, %rsi
    xorq %rdx, %rdx
    syscall
    cmpq $1, %rax
    jne 1f
    movq $42, %rdi
    movq $231, %rax
    syscall
3:
    movq $3, %rax
    movq %r13, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $33, %rax
    movq %r14, %rdi
    movq $1, %rsi
    syscall
    cmpq $1, %rax
    jne 1f
    movq $3, %rax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    leaq pipe_part_one(%rip), %rax
    movq %rax, -64(%rsp)
    movq $4, -56(%rsp)
    leaq pipe_part_two(%rip), %rax
    movq %rax, -48(%rsp)
    movq $3, -40(%rsp)
    movq $20, %rax
    movq $1, %rdi
    leaq -64(%rsp), %rsi
    movq $2, %rdx
    syscall
    cmpq $7, %rax
    jne 1f
    movq $7, %rdi
    movq $231, %rax
    syscall
4:
    movq $3, %rax
    movq %r13, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $1, %rax
    movq %r14, %rdi
    leaq pipe_buffer(%rip), %rsi
    movq $1, %rdx
    syscall
    cmpq $1, %rax
    jne 1f
    movq $9, %rdi
    movq $231, %rax
    syscall
1:
    movq $1, %rdi
    movq $231, %rax
    syscall
    ud2

.section .rodata
expected_path:
    .asciz "/tests/exec-test"
expected_argument:
    .asciz "ok"
exec_test_path:
    .asciz "/tests/exec-test"
dynamic_test_path:
    .asciz "/tests/pie-test"
dynamic_test_argument:
    .asciz "pie"
pipe_part_one:
    .ascii "pipe"
pipe_part_two:
    .ascii "-ok"

.section .data
.balign 8
data_sentinel:
    .quad 1

.section .bss
.balign 8
fs_base_value:
    .skip 8
stat_buffer:
    .skip 144
pipe_descriptors:
    .skip 8
pipe_buffer:
    .skip 32
file_map_buffer:
    .skip 8

.section .note.GNU-stack,"",@progbits
