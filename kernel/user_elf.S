.section .text
.global _start
.type _start, @function
_start:
    testq $15, %rsp
    jne 1f
    cmpq $1, 0(%rsp)
    jne 1f
    movq 8(%rsp), %rax
    testq %rax, %rax
    je 1f
    /* argv[0] is the launch path; AT_EXECFN must point at the same string. */
    cmpb $0, 0(%rax)
    je 1f
    cmpq $0, 16(%rsp)
    jne 1f
    cmpq $0, 24(%rsp)
    jne 1f
    cmpq $3, 32(%rsp)
    jne 1f
    cmpq $0x00fff040, 40(%rsp)
    jne 1f
    movq 40(%rsp), %rbx
    cmpl $1, 0(%rbx)
    jne 1f
    cmpl $4, 4(%rbx)
    jne 1f
    cmpq $0x00fff000, 16(%rbx)
    jne 1f
    cmpl $1, 56(%rbx)
    jne 1f
    cmpl $5, 60(%rbx)
    jne 1f
    cmpq $0x01000000, 72(%rbx)
    jne 1f
    cmpl $1, 112(%rbx)
    jne 1f
    cmpl $4, 116(%rbx)
    jne 1f
    movq 128(%rbx), %rax
    testq $4095, %rax
    jne 1f
    cmpl $1, 168(%rbx)
    jne 1f
    cmpl $6, 172(%rbx)
    jne 1f
    cmpq $0x01100000, 184(%rbx)
    jne 1f
    cmpq $4, 48(%rsp)
    jne 1f
    cmpq $56, 56(%rsp)
    jne 1f
    cmpq $5, 64(%rsp)
    jne 1f
    cmpq $4, 72(%rsp)
    jne 1f
    cmpq $6, 80(%rsp)
    jne 1f
    cmpq $4096, 88(%rsp)
    jne 1f
    cmpq $9, 96(%rsp)
    jne 1f
    leaq _start(%rip), %rax
    cmpq %rax, 104(%rsp)
    jne 1f
    cmpq $11, 112(%rsp)
    jne 1f
    cmpq $0, 120(%rsp)
    jne 1f
    cmpq $12, 128(%rsp)
    jne 1f
    cmpq $0, 136(%rsp)
    jne 1f
    cmpq $13, 144(%rsp)
    jne 1f
    cmpq $0, 152(%rsp)
    jne 1f
    cmpq $14, 160(%rsp)
    jne 1f
    cmpq $0, 168(%rsp)
    jne 1f
    cmpq $23, 176(%rsp)
    jne 1f
    cmpq $0, 184(%rsp)
    jne 1f
    cmpq $25, 192(%rsp)
    jne 1f
    movq 200(%rsp), %rax
    testq $15, %rax
    jne 1f
    cmpq $0x7ffff000, %rax
    jb 1f
    cmpq $0x7ffffff0, %rax
    ja 1f
    movq 0(%rax), %rbx
    orq 8(%rax), %rbx
    je 1f
    cmpq $31, 208(%rsp)
    jne 1f
    movq 216(%rsp), %rax
    cmpq 8(%rsp), %rax
    jne 1f
    cmpq $15, 224(%rsp)
    jne 1f
    movq 232(%rsp), %rax
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
    cmpq $16, 240(%rsp)
    jne 1f
    movl 248(%rsp), %eax
    andl $0x07808101, %eax
    cmpl $0x07808101, %eax
    jne 1f
    cmpq $26, 256(%rsp)
    jne 1f
    cmpq $0, 264(%rsp)
    jne 1f
    cmpq $17, 272(%rsp)
    jne 1f
    cmpq $100, 280(%rsp)
    jne 1f
    cmpq $8, 288(%rsp)
    jne 1f
    cmpq $0, 296(%rsp)
    jne 1f
    cmpq $0, 304(%rsp)
    jne 1f
    cmpq $0, 312(%rsp)
    jne 1f

    jmp page_boundary_test
    .fill 5000, 1, 0x90
page_boundary_test:
    movq initialized_value(%rip), %rax
    cmpq $41, %rax
    jne 1f
    cmpq $0, zero_initialized_value(%rip)
    jne 1f
    cmpq $0, zero_initialized_tail(%rip)
    jne 1f
    incq initialized_value(%rip)
    movq $0x4f504f53, zero_initialized_value(%rip)
    movq $0x44415441, zero_initialized_tail(%rip)
    cmpq $42, initialized_value(%rip)
    jne 1f
    cmpq $0x4f504f53, zero_initialized_value(%rip)
    jne 1f
    cmpq $0x44415441, zero_initialized_tail(%rip)
    jne 1f

    leaq -32(%rsp), %r13
    leaq message(%rip), %rax
    movq %rax, 0(%r13)
    movq $(message_split - message), 8(%r13)
    leaq message_split(%rip), %rax
    movq %rax, 16(%r13)
    movq $(message_end - message_split), 24(%r13)
    movq $20, %rax
    movq $1, %rdi
    movq %r13, %rsi
    movq $2, %rdx
    syscall
    cmpq $(message_end - message), %rax
    jne 1f

    movq $39, %rax
    syscall
    testq %rax, %rax
    jz 1f

    movq $158, %rax
    movq $0x1002, %rdi
    leaq initialized_value(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq %fs:0, %rax
    cmpq $42, %rax
    jne 1f

    movq $158, %rax
    movq $0x1003, %rdi
    leaq -8(%rsp), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq -8(%rsp), %rax
    leaq initialized_value(%rip), %rcx
    cmpq %rcx, %rax
    jne 1f

    movq $158, %rax
    movq $0x1002, %rdi
    movabsq $0x50000000, %rsi
    syscall
    cmpq $-14, %rax
    jne 1f

    movq $158, %rax
    movq $0x1003, %rdi
    movabsq $0x50000000, %rsi
    syscall
    cmpq $-14, %rax
    jne 1f

    movq $158, %rax
    movq $0x1002, %rdi
    xorq %rsi, %rsi
    syscall
    testq %rax, %rax
    jne 1f

    movq $63, %rax
    leaq utsname_buffer(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movabsq $0x000078756e694c, %rax
    cmpq %rax, utsname_buffer(%rip)
    jne 1f
    cmpl $0x736f706f, utsname_buffer+65(%rip)
    jne 1f
    cmpb $0, utsname_buffer+69(%rip)
    jne 1f
    movabsq $0x706f2d302e312e30, %rax
    cmpq %rax, utsname_buffer+130(%rip)
    jne 1f
    movabsq $0x00534f504f203123, %rax
    cmpq %rax, utsname_buffer+195(%rip)
    jne 1f
    movabsq $0x000034365f363878, %rax
    cmpq %rax, utsname_buffer+260(%rip)
    jne 1f
    movabsq $0x6d6f646c61636f6c, %rax
    cmpq %rax, utsname_buffer+325(%rip)
    jne 1f
    cmpw $0, utsname_buffer+336(%rip)
    jne 1f

    movq $102, %rax
    syscall
    testq %rax, %rax
    jne 1f
    movq $104, %rax
    syscall
    testq %rax, %rax
    jne 1f
    movq $107, %rax
    syscall
    testq %rax, %rax
    jne 1f
    movq $108, %rax
    syscall
    testq %rax, %rax
    jne 1f

    movq $96, %rax
    leaq -16(%rsp), %rdi
    leaq -32(%rsp), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    cmpq $0, -16(%rsp)
    js 1f
    cmpq $0, -8(%rsp)
    js 1f
    cmpq $1000000, -8(%rsp)
    jae 1f
    cmpl $0, -32(%rsp)
    jne 1f
    cmpl $0, -28(%rsp)
    jne 1f

    movq $96, %rax
    xorq %rdi, %rdi
    xorq %rsi, %rsi
    syscall
    testq %rax, %rax
    jne 1f

    movabsq $0x123456789abcdef0, %rax
    movq %rax, -16(%rsp)
    movq $96, %rax
    leaq -16(%rsp), %rdi
    movabsq $0x50000000, %rsi
    syscall
    cmpq $-14, %rax
    jne 1f
    movabsq $0x123456789abcdef0, %rax
    cmpq %rax, -16(%rsp)
    jne 1f

    movq $96, %rax
    movabsq $0x50000000, %rdi
    xorq %rsi, %rsi
    syscall
    cmpq $-14, %rax
    jne 1f

    movq $63, %rax
    movabsq $0x50000000, %rdi
    syscall
    cmpq $-14, %rax
    jne 1f

    movq $158, %rax
    movq $0x1004, %rdi
    xorq %rsi, %rsi
    syscall
    cmpq $-22, %rax
    jne 1f

    movq $228, %rax
    movq $1, %rdi
    leaq -16(%rsp), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq -16(%rsp), %r12
    movq -8(%rsp), %r13
    testq %r12, %r12
    js 1f
    testq %r13, %r13
    js 1f
    cmpq $1000000000, %r13
    jae 1f

    movq %r12, %r14
    movq %r13, %r15
    addq $20000000, %r15
    cmpq $1000000000, %r15
    jb 3f
    subq $1000000000, %r15
    incq %r14
3:
    movq $0, -40(%rsp)
    movq $20000000, -32(%rsp)
    movq $35, %rax
    leaq -40(%rsp), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $228, %rax
    movq $1, %rdi
    leaq -16(%rsp), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    cmpq %r14, -16(%rsp)
    jb 1f
    ja 4f
    cmpq %r15, -8(%rsp)
    jb 1f
4:
    movq $0, -40(%rsp)
    movq $0, -32(%rsp)
    movq $35, %rax
    leaq -40(%rsp), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $0, -40(%rsp)
    movq $1000000000, -32(%rsp)
    movq $35, %rax
    leaq -40(%rsp), %rdi
    syscall
    cmpq $-22, %rax
    jne 1f
    movq $35, %rax
    movabsq $0x50000000, %rdi
    syscall
    cmpq $-14, %rax
    jne 1f
    movq $228, %rax
    movq $1, %rdi
    leaq -16(%rsp), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    cmpq -16(%rsp), %r12
    ja 1f
    jb 2f
    cmpq -8(%rsp), %r13
    ja 1f
2:
    movq $229, %rax
    movq $1, %rdi
    leaq -16(%rsp), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    cmpq $0, -16(%rsp)
    jne 1f
    cmpq $0, -8(%rsp)
    jbe 1f
    cmpq $1000000000, -8(%rsp)
    jae 1f

    movq $229, %rax
    movq $1, %rdi
    xorq %rsi, %rsi
    syscall
    testq %rax, %rax
    jne 1f

    movq $228, %rax
    xorq %rdi, %rdi
    leaq -16(%rsp), %rsi
    syscall
    cmpq $-22, %rax
    jne 1f

    movq $228, %rax
    movq $1, %rdi
    movabsq $0x50000000, %rsi
    syscall
    cmpq $-14, %rax
    jne 1f

    movq $20, %rax
    movq $1, %rdi
    movabsq $0x50000000, %rsi
    movq $1, %rdx
    syscall
    cmpq $-14, %rax
    jne 1f

    movq $1, %rax
    movq $1, %rdi
    movabsq $0x50000000, %rsi
    movq $1, %rdx
    syscall
    cmpq $-14, %rax
    jne 1f

    movq $2, %rax
    leaq initramfs_elf_path(%rip), %rdi
    xorq %rsi, %rsi
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %rbx
    xorq %rax, %rax
    movq %rbx, %rdi
    leaq mount_buffer(%rip), %rsi
    movq $4, %rdx
    syscall
    cmpq $4, %rax
    jne 1f
    cmpl $0x464c457f, mount_buffer(%rip)
    jne 1f
    movq $32, %rax
    movq %rbx, %rdi
    syscall
    cmpq $3, %rax
    jle 1f
    movq %rax, %r14
    movq $2, %rax
    leaq root_path(%rip), %rdi
    movq $65536, %rsi
    syscall
    cmpq $5, %rax
    jne 1f
    movq $33, %rax
    movq %r14, %rdi
    movq $5, %rsi
    syscall
    cmpq $5, %rax
    jne 1f
    movq $292, %rax
    movq %r14, %rdi
    movq $8, %rsi
    movq $524288, %rdx
    syscall
    cmpq $8, %rax
    jne 1f
    movq $292, %rax
    movq %r14, %rdi
    movq %r14, %rsi
    xorq %rdx, %rdx
    syscall
    cmpq $-22, %rax
    jne 1f
    xorq %rax, %rax
    movq %r14, %rdi
    leaq mount_buffer(%rip), %rsi
    movq $1, %rdx
    syscall
    cmpq $1, %rax
    jne 1f
    cmpb $2, mount_buffer(%rip)
    jne 1f
    xorq %rax, %rax
    movq %rbx, %rdi
    leaq mount_buffer(%rip), %rsi
    movq $1, %rdx
    syscall
    cmpq $1, %rax
    jne 1f
    cmpb $1, mount_buffer(%rip)
    jne 1f
    xorq %rax, %rax
    movq $8, %rdi
    leaq mount_buffer(%rip), %rsi
    movq $1, %rdx
    syscall
    cmpq $1, %rax
    jne 1f
    cmpb $1, mount_buffer(%rip)
    jne 1f
    movq $3, %rax
    movq %rbx, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $3, %rax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $5, %rax
    movq $5, %rdi
    leaq mount_buffer(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    cmpq $1, mount_buffer(%rip)
    jne 1f
    cmpq $0, mount_buffer+48(%rip)
    jle 1f
    movq $3, %rax
    movq $5, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $3, %rax
    movq $8, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $2, %rax
    leaq initramfs_elf_path(%rip), %rdi
    movq $1, %rsi
    syscall
    cmpq $-30, %rax
    jne 1f
    movq $2, %rax
    leaq read_only_new_path(%rip), %rdi
    movq $65, %rsi
    syscall
    cmpq $-30, %rax
    jne 1f
    movq $83, %rax
    leaq read_only_directory_path(%rip), %rdi
    movq $0777, %rsi
    syscall
    cmpq $-30, %rax
    jne 1f
    movq $87, %rax
    leaq initramfs_elf_path(%rip), %rdi
    syscall
    cmpq $-30, %rax
    jne 1f
    movq $82, %rax
    leaq initramfs_elf_path(%rip), %rdi
    leaq fat_rename_path(%rip), %rsi
    syscall
    cmpq $-30, %rax
    jne 1f

    movq $2, %rax
    leaq root_path(%rip), %rdi
    movq $65536, %rsi
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %rbx
    movq $217, %rax
    movq %rbx, %rdi
    leaq mount_buffer(%rip), %rsi
    movq $512, %rdx
    syscall
    cmpq $120, %rax
    je 8f
    cmpq $96, %rax
    jne 1f
    cmpl $0x006e6962, mount_buffer+67(%rip)
    jne 1f
    cmpl $0x0062696c, mount_buffer+91(%rip)
    jne 1f
    jmp 9f
8:
    cmpl $0x6b736964, mount_buffer+67(%rip)
    jne 1f
    cmpl $0x006e6962, mount_buffer+91(%rip)
    jne 1f
    cmpl $0x0062696c, mount_buffer+115(%rip)
    jne 1f
9:
    movq $4, %rax
    leaq fat_root_path(%rip), %rdi
    leaq mount_buffer(%rip), %rsi
    syscall
    testq %rax, %rax
    je 7f
    cmpq $-2, %rax
    jne 1f
    jmp 6f
7:
    cmpq $2, mount_buffer(%rip)
    jne 1f
6:
    movq $3, %rax
    movq %rbx, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $80, %rax
    leaq root_path(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $79, %rax
    leaq mount_buffer(%rip), %rdi
    movq $512, %rsi
    syscall
    cmpq $2, %rax
    jne 1f
    cmpw $0x002f, mount_buffer(%rip)
    jne 1f

    movq $12, %rax
    xorq %rdi, %rdi
    syscall
    movq %rax, %r12
    leaq 8192(%r12), %rdi
    movq $12, %rax
    syscall
    cmpq %rdi, %rax
    jne 1f
    cmpq $0, -8(%rdi)
    jne 1f
    movabsq $0x4f504f5348454150, %rax
    movq %rax, -8(%rdi)
    cmpq %rax, -8(%rdi)
    jne 1f

    movq %r12, %rdi
    movq $12, %rax
    syscall
    cmpq %r12, %rax
    jne 1f
    leaq 4095(%r12), %rdi
    andq $-4096, %rdi
    addq $(128 * 4096 + 1), %rdi
    movq $12, %rax
    syscall
    cmpq %r12, %rax
    jne 1f

    movq $9, %rax
    xorq %rdi, %rdi
    movq $8192, %rsi
    movq $3, %rdx
    movq $0x22, %r10
    movq $-1, %r8
    xorq %r9, %r9
    syscall
    cmpq $0x10000000, %rax
    jb 1f
    testq $4095, %rax
    jne 1f
    movq %rax, %r13
    cmpq $0, (%r13)
    jne 1f
    cmpq $0, 4096(%r13)
    jne 1f

    movb $0xc3, 8(%r13)
    movb $0xc3, 4104(%r13)
    movq $10, %rax
    movq %r13, %rdi
    movq $8192, %rsi
    movq $7, %rdx
    syscall
    cmpq $-22, %rax
    jne 1f

    movq $10, %rax
    movq %r13, %rdi
    movq $8192, %rsi
    xorq %rdx, %rdx
    syscall
    testq %rax, %rax
    jne 1f

    movq $10, %rax
    leaq 1(%r13), %rdi
    movq $4096, %rsi
    movq $1, %rdx
    syscall
    cmpq $-22, %rax
    jne 1f

    movq $10, %rax
    movq %r13, %rdi
    movq $12288, %rsi
    movq $1, %rdx
    syscall
    cmpq $-12, %rax
    jne 1f

    movq $10, %rax
    movq %r13, %rdi
    movq $8192, %rsi
    movq $5, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    leaq 8(%r13), %rax
    call *%rax
    leaq 4104(%r13), %rax
    call *%rax

    movq $10, %rax
    movq %r13, %rdi
    movq $8192, %rsi
    movq $7, %rdx
    syscall
    cmpq $-22, %rax
    jne 1f
    leaq 8(%r13), %rax
    call *%rax

    movq $10, %rax
    movq %r13, %rdi
    movq $8192, %rsi
    movq $1, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    movq $10, %rax
    movq %r13, %rdi
    movq $8192, %rsi
    movq $3, %rdx
    syscall
    testq %rax, %rax
    jne 1f

    movabsq $0x4d4d415048454150, %rax
    movq %rax, 4096(%r13)
    cmpq %rax, 4096(%r13)
    jne 1f

    movq $11, %rax
    movq %r13, %rdi
    movq $8192, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq $1, %rax
    movq $1, %rdi
    movq %r13, %rsi
    movq $1, %rdx
    syscall
    cmpq $-14, %rax
    jne 1f

    movq $11, %rax
    movq %r13, %rdi
    movq $4096, %rsi
    syscall
    cmpq $-22, %rax
    jne 1f

    movq $9, %rax
    xorq %rdi, %rdi
    movq $(16384 * 4096 + 1), %rsi
    movq $3, %rdx
    movq $0x22, %r10
    movq $-1, %r8
    xorq %r9, %r9
    syscall
    cmpq $-12, %rax
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
    cmpq $0, (%rax)
    jne 1f

    xorq %rdi, %rdi
    movq $12, %rax
    syscall
    cmpq %r12, %rax
    jne 1f

    movq $2, %rax
    leaq root_path(%rip), %rdi
    movq $65536, %rsi
    syscall
    cmpq $3, %rax
    jne 1f
    movq $2, %rax
    leaq exec_test_path(%rip), %rdi
    movq $524288, %rsi
    syscall
    cmpq $4, %rax
    jne 1f
    leaq exec_test_path(%rip), %rax
    movq %rax, -32(%rsp)
    leaq exec_test_argument(%rip), %rax
    movq %rax, -24(%rsp)
    movq $0, -16(%rsp)
    movq $0, -8(%rsp)
    movq $59, %rax
    leaq exec_test_path(%rip), %rdi
    leaq -32(%rsp), %rsi
    leaq -8(%rsp), %rdx
    syscall
    negq %rax
    movq %rax, %rdi
    movq $231, %rax
    syscall
1:
    movq $1, %rdi
    movq $231, %rax
    syscall
    ud2

.section .rodata
message:
    .ascii "OPOS: ELF64 R/RX/R/RW "
message_split:
    .ascii "image loaded\n"
message_end:
initramfs_elf_path:
    .asciz "/bin/elf-test"
root_path:
    .asciz "/"
read_only_new_path:
    .asciz "/new"
read_only_directory_path:
    .asciz "/newdir"
fat_rename_path:
    .asciz "/disk/ELF.TMP"
fat_root_path:
    .asciz "/disk"
exec_test_path:
    .asciz "/bin/exec-test"
exec_test_argument:
    .asciz "ok"

.section .data
.balign 8
initialized_value:
    .quad 41

.section .bss
.balign 8
zero_initialized_value:
    .skip 8
    .skip 5000
zero_initialized_tail:
    .quad 0
utsname_buffer:
    .skip 390
mount_buffer:
    .skip 512

.section .note.GNU-stack,"",@progbits
