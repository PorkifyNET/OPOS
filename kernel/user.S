.section .text

.global gdt_load
.type gdt_load, @function
gdt_load:
    lgdt (%rdi)
    pushq $0x28
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    movw $0x30, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    ret

.global tss_load
.type tss_load, @function
tss_load:
    ltr %di
    ret

.section .text
.global user_enter
.type user_enter, @function
user_enter:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rdi, %r12
    movq %rsi, %r13
    movq %rsp, %rdi
    call scheduler_set_user_return_stack

    movq %r12, %rax
    pushq $0x3b
    pushq %r13
    pushq $0x202
    pushq $0x43
    pushq %rax
    iretq

.global user_exit_trampoline
.type user_exit_trampoline, @function
user_exit_trampoline:
    movq %rax, %rbx
    andq $-16, %rsp
    call scheduler_user_return_stack
    movq %rax, %r13
    movq %rbx, %rax
    pushq %rax
    xorl %eax, %eax
    xorl %edx, %edx
    movl $0xc0000100, %ecx
    wrmsr
    popq %rax
    movq %r13, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

.global user_linux_resume
.type user_linux_resume, @function
user_linux_resume:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rdi, %r12
    movq %rsi, %r13
    movq %rsp, %rdi
    call scheduler_set_user_return_stack

    movq 120(%r12), %rax
    movq %rax, %rdx
    shrq $32, %rdx
    movl $0xc0000100, %ecx
    wrmsr

    pushq $0x3b
    pushq %r13
    pushq 112(%r12)
    pushq $0x43
    pushq 104(%r12)
    movq %r12, %r11
    movq 0(%r11), %r15
    movq 8(%r11), %r14
    movq 16(%r11), %r13
    movq 24(%r11), %r12
    movq 32(%r11), %rbp
    movq 48(%r11), %r9
    movq 56(%r11), %r8
    movq 64(%r11), %r10
    movq 72(%r11), %rdx
    movq 80(%r11), %rsi
    movq 88(%r11), %rdi
    movq 96(%r11), %rax
    movq 40(%r11), %rbx
    iretq

.section .user_program,"a"
.global user_program_start
.global user_program_end
user_program_start:
    movq $1, %rax
    movq $1, %rdi
    movabsq $0x50000000, %rsi
    movq $1, %rdx
    int $0x80
    cmpq $-14, %rax
    jne 1f

    movq $1, %rax
    movq $1, %rdi
    leaq user_program_message(%rip), %rsi
    movq $(user_program_message_end - user_program_message), %rdx
    int $0x80
    testq %rax, %rax
    js 1f
    xorq %rdi, %rdi
    jmp 2f
1:
    movq $1, %rdi
2:
    movq $2, %rax
    int $0x80
    ud2

user_program_message:
    .ascii "OPOS: hello from ring 3 via syscall\n"
user_program_message_end:
user_program_end:

.global user_fault_program_start
.global user_fault_program_end
user_fault_program_start:
    movabsq $0x00400000, %rdi
    movb $0, (%rdi)
    movq $2, %rax
    movq $1, %rdi
    int $0x80
    ud2
user_fault_program_end:

.global user_nx_program_start
.global user_nx_program_end
user_nx_program_start:
    movabsq $0x7ffffff0, %rax
    jmp *%rax
user_nx_program_end:

.global user_input_program_start
.global user_input_program_end
user_input_program_start:
    movq $1, %rax
    movq $1, %rdi
    leaq user_input_prompt(%rip), %rsi
    movq $(user_input_prompt_end - user_input_prompt), %rdx
    int $0x80

    movq $3, %rax
    xorq %rdi, %rdi
    movabsq $0x7fffffe0, %rsi
    movq $32, %rdx
    int $0x80
    testq %rax, %rax
    jle 1f
    xorq %rdi, %rdi
    jmp 2f
1:
    movq $1, %rdi
2:
    movq $2, %rax
    int $0x80
    ud2

user_input_prompt:
    .ascii "OPOS: enter a line: "
user_input_prompt_end:
user_input_program_end:

.global user_shell_program_start
.global user_shell_program_end
user_shell_program_start:
1:
    movq $1, %rax
    movq $1, %rdi
    leaq user_shell_prompt(%rip), %rsi
    movq $(user_shell_prompt_end - user_shell_prompt), %rdx
    int $0x80

    movq $3, %rax
    xorq %rdi, %rdi
    movabsq $0x7ffffe00, %rsi
    movq $64, %rdx
    int $0x80
    testq %rax, %rax
    jle 3f
    movq %rax, %r8
    cmpq $5, %r8
    jne 2f
    cmpb $'e', 0x7ffffe00
    jne 2f
    cmpb $'x', 0x7ffffe01
    jne 2f
    cmpb $'i', 0x7ffffe02
    jne 2f
    cmpb $'t', 0x7ffffe03
    jne 2f
    cmpb $'\n', 0x7ffffe04
    je 3f

2:
    cmpq $5, %r8
    jne 4f
    cmpb $'h', 0x7ffffe00
    jne 4f
    cmpb $'e', 0x7ffffe01
    jne 4f
    cmpb $'l', 0x7ffffe02
    jne 4f
    cmpb $'p', 0x7ffffe03
    jne 4f
    cmpb $'\n', 0x7ffffe04
    jne 4f
    movq $1, %rax
    movq $1, %rdi
    leaq user_shell_help(%rip), %rsi
    movq $(user_shell_help_end - user_shell_help), %rdx
    int $0x80
    jmp 1b

4:
    cmpq $5, %r8
    jb 5f
    cmpb $'e', 0x7ffffe00
    jne 5f
    cmpb $'c', 0x7ffffe01
    jne 5f
    cmpb $'h', 0x7ffffe02
    jne 5f
    cmpb $'o', 0x7ffffe03
    jne 5f
    cmpb $' ', 0x7ffffe04
    jne 5f
    movq $1, %rax
    movq $1, %rdi
    movabsq $0x7ffffe05, %rsi
    leaq -5(%r8), %rdx
    int $0x80
    jmp 1b

5:
    movq $1, %rax
    movq $1, %rdi
    leaq user_shell_unknown(%rip), %rsi
    movq $(user_shell_unknown_end - user_shell_unknown), %rdx
    int $0x80
    jmp 1b
3:
    xorq %rdi, %rdi
    movq $2, %rax
    int $0x80
    ud2

user_shell_prompt:
    .ascii "user> "
user_shell_prompt_end:
user_shell_help:
    .ascii "Commands: help, echo, exit\n"
user_shell_help_end:
user_shell_unknown:
    .ascii "Unknown command\n"
user_shell_unknown_end:
user_shell_program_end:

.global user_linux_read_program_start
.global user_linux_read_program_end
user_linux_read_program_start:
    movq $1, %rax
    movq $1, %rdi
    leaq linux_read_prompt(%rip), %rsi
    movq $(linux_read_prompt_end - linux_read_prompt), %rdx
    syscall
    cmpq $(linux_read_prompt_end - linux_read_prompt), %rax
    jne 1f

    xorq %rax, %rax
    xorq %rdi, %rdi
    movabsq $0x7fffffe0, %rsi
    movq $32, %rdx
    syscall
    testq %rax, %rax
    jle 1f
    movq %rax, %r12

    movq $1, %rax
    movq $1, %rdi
    movabsq $0x7fffffe0, %rsi
    movq %r12, %rdx
    syscall
    cmpq %r12, %rax
    jne 1f
    xorq %rdi, %rdi
    jmp 2f
1:
    movq $1, %rdi
2:
    movq $60, %rax
    syscall
    ud2

linux_read_prompt:
    .ascii "OPOS: Linux read syscall; type a line: "
linux_read_prompt_end:
user_linux_read_program_end:

.global user_preempt_program_start
.global user_preempt_program_end
user_preempt_program_start:
    movq $39, %rax
    syscall
    testq %rax, %rax
    jle 2f
    movq %rax, %r13
    movq %rax, %xmm0
    movabsq $0x7fffffc0, %rdi
    movq $0, 0(%rdi)
    movq $200000000, 8(%rdi)
    movq $35, %rax
    xorq %rsi, %rsi
    syscall
    testq %rax, %rax
    jne 2f
    movq %xmm0, %r12
    cmpq %r13, %r12
    jne 2f

    movq $1, %rax
    movq $1, %rdi
    leaq user_preempt_message(%rip), %rsi
    movq $(user_preempt_message_end - user_preempt_message), %rdx
    syscall
    testq %rax, %rax
    js 2f
    xorq %rdi, %rdi
    jmp 3f
2:
    movq $1, %rdi
3:
    movq $60, %rax
    syscall
    ud2

user_preempt_message:
    .ascii "OPOS: concurrent ring-3 task finished\n"
user_preempt_message_end:
user_preempt_program_end:

.global user_spawn_program_start
.global user_spawn_program_end
user_spawn_program_start:
    movq $4, %rax
    xorq %rdi, %rdi
    int $0x80
    testq %rax, %rax
    jle 1f
    movq %rax, %r12

    movq $4, %rax
    movq $1, %rdi
    leaq user_spawn_child_path(%rip), %rsi
    movq $(user_spawn_child_path_end - user_spawn_child_path), %rdx
    int $0x80
    testq %rax, %rax
    jle 1f
    movq %rax, %r13

    movq $5, %rax
    movq %r12, %rdi
    movabsq $0x7fffffc0, %rsi
    int $0x80
    testq %rax, %rax
    jne 1f
    movq 0(%rsi), %r14
    testq %r14, %r14
    je 1f

    movq $5, %rax
    movq %r13, %rdi
    movabsq $0x7fffffc8, %rsi
    int $0x80
    testq %rax, %rax
    jne 1f
    cmpq %r14, 0(%rsi)
    je 1f

    movq $1, %rax
    movq $1, %rdi
    leaq user_spawn_message(%rip), %rsi
    movq $(user_spawn_message_end - user_spawn_message), %rdx
    int $0x80
    testq %rax, %rax
    js 1f
    xorq %rdi, %rdi
    jmp 2f
1:
    movq $1, %rdi
2:
    movq $2, %rax
    int $0x80
    ud2

user_spawn_message:
    .ascii "OPOS: ring-3 spawn/wait passed\n"
user_spawn_message_end:
user_spawn_child_path:
    .ascii "/bin/elf-test"
user_spawn_child_path_end:
user_spawn_program_end:

.global user_file_program_start
.global user_file_program_end
user_file_program_start:
    movq $80, %rax
    leaq user_disk_absolute_path(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $79, %rax
    movabsq $0x7fffff80, %rdi
    movq $64, %rsi
    syscall
    cmpq $11, %rax
    jne 1f
    movabsq $0x4f442f6b7369642f, %rax
    cmpq %rax, 0x7fffff80
    jne 1f
    cmpw $0x5343, 0x7fffff88
    jne 1f
    cmpb $0, 0x7fffff8a
    jne 1f

    movq $79, %rax
    movabsq $0x7fffff80, %rdi
    movq $10, %rsi
    syscall
    cmpq $-34, %rax
    jne 1f

    movq $76, %rax
    leaq user_created_file_path(%rip), %rdi
    movq $100, %rsi
    syscall
    testq %rax, %rax
    jne 1f

    movq $76, %rax
    leaq user_disk_path(%rip), %rdi
    movq $1, %rsi
    syscall
    cmpq $-21, %rax
    jne 1f

    movq $257, %rax
    movq $-100, %rdi
    leaq user_truncate_file_path(%rip), %rsi
    movq $578, %rdx
    movq $0644, %r10
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %rbx
    movq $1, %rax
    movq %rbx, %rdi
    leaq user_file_data(%rip), %rsi
    movq $(user_file_data_end - user_file_data), %rdx
    syscall
    cmpq $(user_file_data_end - user_file_data), %rax
    jne 1f

    movq $77, %rax
    movq %rbx, %rdi
    movq $600, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    xorq %rax, %rax
    movq %rbx, %rdi
    movabsq $0x7fffff00, %rsi
    movq $32, %rdx
    syscall
    cmpq $32, %rax
    jne 1f
    movabsq $0x7fffff00, %rdi
    movq $32, %rcx
    xorq %rax, %rax
    repe scasb
    jne 1f

    movq $77, %rax
    movq %rbx, %rdi
    movq $15, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq $5, %rax
    movq %rbx, %rdi
    movabsq $0x7ffffe00, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    cmpq $15, 0x7ffffe30
    jne 1f
    movq 0x7ffffe08, %r15

    movq $82, %rax
    leaq user_truncate_file_path(%rip), %rdi
    leaq user_moved_file_path(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq $316, %rax
    movq $-100, %rdi
    leaq user_moved_file_name(%rip), %rsi
    movq $-100, %rdx
    leaq user_renamed_file_name(%rip), %r10
    xorq %r8, %r8
    syscall
    testq %rax, %rax
    jne 1f
    movq $82, %rax
    leaq user_renamed_file_path(%rip), %rdi
    leaq user_truncate_file_path(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq $5, %rax
    movq %rbx, %rdi
    movabsq $0x7ffffe00, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    cmpq 0x7ffffe08, %r15
    jne 1f
    movq $3, %rax
    movq %rbx, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $257, %rax
    movq $-100, %rdi
    leaq user_rename_source(%rip), %rsi
    movq $578, %rdx
    movq $0644, %r10
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %rdi
    movq $3, %rax
    syscall
    testq %rax, %rax
    jne 1f
    movq $257, %rax
    movq $-100, %rdi
    leaq user_rename_target(%rip), %rsi
    movq $578, %rdx
    movq $0644, %r10
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %rbx
    movq $1, %rax
    movq %rbx, %rdi
    leaq user_file_data(%rip), %rsi
    movq $(user_file_data_end - user_file_data), %rdx
    syscall
    cmpq $(user_file_data_end - user_file_data), %rax
    jne 1f
    movq $264, %rax
    movq $-100, %rdi
    leaq user_rename_source(%rip), %rsi
    movq $-100, %rdx
    leaq user_rename_target(%rip), %r10
    syscall
    testq %rax, %rax
    jne 1f
    movq $8, %rax
    movq %rbx, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    xorq %rax, %rax
    movq %rbx, %rdi
    movabsq $0x7fffffc0, %rsi
    movq $(user_file_data_end - user_file_data), %rdx
    syscall
    cmpq $(user_file_data_end - user_file_data), %rax
    jne 1f
    leaq user_file_data(%rip), %rsi
    movabsq $0x7fffffc0, %rdi
    movq $(user_file_data_end - user_file_data), %rcx
    repe cmpsb
    jne 1f
    movq $3, %rax
    movq %rbx, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $87, %rax
    leaq user_rename_target(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $257, %rax
    movq $-100, %rdi
    leaq user_file_path(%rip), %rsi
    movq $578, %rdx
    movq $0644, %r10
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %r12

    movq $1, %rax
    movq %r12, %rdi
    leaq user_file_data(%rip), %rsi
    movq $(user_file_data_end - user_file_data), %rdx
    syscall
    cmpq $(user_file_data_end - user_file_data), %rax
    jne 1f

    movq $8, %rax
    movq %r12, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    syscall
    testq %rax, %rax
    jne 1f

    xorq %rax, %rax
    movq %r12, %rdi
    movabsq $0x7fffffc0, %rsi
    movq $32, %rdx
    syscall
    cmpq $(user_file_data_end - user_file_data), %rax
    jne 1f
    leaq user_file_data(%rip), %rsi
    movabsq $0x7fffffc0, %rdi
    movq $(user_file_data_end - user_file_data), %rcx
    repe cmpsb
    jne 1f

    movq $8, %rax
    movq %r12, %rdi
    movq $-4, %rsi
    movq $2, %rdx
    syscall
    cmpq $(user_file_data_end - user_file_data - 4), %rax
    jne 1f
    xorq %rax, %rax
    movq %r12, %rdi
    movabsq $0x7fffffc0, %rsi
    movq $4, %rdx
    syscall
    cmpq $4, %rax
    jne 1f
    cmpl $0x61746164, 0x7fffffc0
    jne 1f

    movq $8, %rax
    movq %r12, %rdi
    movq $-100, %rsi
    xorq %rdx, %rdx
    syscall
    cmpq $-22, %rax
    jne 1f

    movq $3, %rax
    movq %r12, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    xorq %rax, %rax
    movq %r12, %rdi
    movabsq $0x7fffffc0, %rsi
    movq $1, %rdx
    syscall
    cmpq $-9, %rax
    jne 1f

    movq $2, %rax
    leaq user_file_path(%rip), %rdi
    xorq %rsi, %rsi
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %r12
    xorq %rax, %rax
    movq %r12, %rdi
    movabsq $0x7fffffc0, %rsi
    movq $32, %rdx
    syscall
    cmpq $(user_file_data_end - user_file_data), %rax
    jne 1f

    movabsq $0x7ffffe00, %r13
    movq $5, %rax
    movq %r12, %rdi
    movq %r13, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movl 24(%r13), %eax
    andl $0170000, %eax
    cmpl $0100000, %eax
    jne 1f
    cmpq $(user_file_data_end - user_file_data), 48(%r13)
    jne 1f
    cmpq $512, 56(%r13)
    jne 1f
    cmpq $1, 64(%r13)
    jne 1f

    movq $4, %rax
    leaq user_file_path(%rip), %rdi
    movq %r13, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    cmpq $(user_file_data_end - user_file_data), 48(%r13)
    jne 1f

    movq $262, %rax
    movq $-100, %rdi
    leaq user_file_path(%rip), %rsi
    movq %r13, %rdx
    xorq %r10, %r10
    syscall
    testq %rax, %rax
    jne 1f
    cmpq $(user_file_data_end - user_file_data), 48(%r13)
    jne 1f

    movq $258, %rax
    movq $-100, %rdi
    leaq user_subdirectory_path(%rip), %rsi
    movq $0777, %rdx
    syscall
    testq %rax, %rax
    je 3f
    cmpq $-17, %rax
    jne 1f
3:
    movq $80, %rax
    leaq user_subdirectory_path(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $80, %rax
    leaq user_parent_path(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $79, %rax
    movabsq $0x7fffff80, %rdi
    movq $64, %rsi
    syscall
    cmpq $11, %rax
    jne 1f
    movabsq $0x4f442f6b7369642f, %rax
    cmpq %rax, 0x7fffff80
    jne 1f
    cmpw $0x5343, 0x7fffff88
    jne 1f
    cmpb $0, 0x7fffff8a
    jne 1f

    movq $84, %rax
    leaq user_disk_path(%rip), %rdi
    syscall
    cmpq $-39, %rax
    jne 1f

    movq $84, %rax
    leaq user_subdirectory_path(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $258, %rax
    movq $-100, %rdi
    leaq user_reused_directory_path(%rip), %rsi
    movq $0777, %rdx
    syscall
    testq %rax, %rax
    je 4f
    cmpq $-17, %rax
    jne 1f
4:
    movq $84, %rax
    leaq user_file_path(%rip), %rdi
    syscall
    cmpq $-20, %rax
    jne 1f
    movq $87, %rax
    leaq user_reused_directory_path(%rip), %rdi
    syscall
    cmpq $-21, %rax
    jne 1f

    movq $257, %rax
    movq $-100, %rdi
    leaq user_deleted_file_path(%rip), %rsi
    movq $578, %rdx
    movq $0644, %r10
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %rbx
    movq $1, %rax
    movq %rbx, %rdi
    leaq user_file_data(%rip), %rsi
    movq $(user_file_data_end - user_file_data), %rdx
    syscall
    cmpq $(user_file_data_end - user_file_data), %rax
    jne 1f
    movq $263, %rax
    movq $-100, %rdi
    leaq user_deleted_file_path(%rip), %rsi
    xorq %rdx, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    movq $8, %rax
    movq %rbx, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    xorq %rax, %rax
    movq %rbx, %rdi
    movabsq $0x7fffffc0, %rsi
    movq $(user_file_data_end - user_file_data), %rdx
    syscall
    cmpq $(user_file_data_end - user_file_data), %rax
    jne 1f
    movq $77, %rax
    movq %rbx, %rdi
    movq $5, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq $5, %rax
    movq %rbx, %rdi
    movabsq $0x7ffffe00, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    cmpq $5, 0x7ffffe30
    jne 1f
    movq $3, %rax
    movq %rbx, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $257, %rax
    movq $-100, %rdi
    leaq user_reused_file_path(%rip), %rsi
    movq $578, %rdx
    movq $0644, %r10
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %rbx
    movq $1, %rax
    movq %rbx, %rdi
    leaq user_file_data(%rip), %rsi
    movq $(user_file_data_end - user_file_data), %rdx
    syscall
    cmpq $(user_file_data_end - user_file_data), %rax
    jne 1f
    movq $3, %rax
    movq %rbx, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $258, %rax
    movq $-100, %rdi
    leaq user_rename_directory_name(%rip), %rsi
    movq $0777, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    movq $257, %rax
    movq $-100, %rdi
    leaq user_rename_directory_name(%rip), %rsi
    movq $65536, %rdx
    xorq %r10, %r10
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %rbx
    movq $82, %rax
    leaq user_disk_absolute_path(%rip), %rdi
    leaq user_rename_cycle_path(%rip), %rsi
    syscall
    cmpq $-22, %rax
    jne 1f
    movq $82, %rax
    leaq user_rename_directory_path(%rip), %rdi
    leaq user_rename_directory_root(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq $258, %rax
    movq %rbx, %rdi
    leaq user_rename_inner_name(%rip), %rsi
    movq $0777, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    movq $263, %rax
    movq %rbx, %rdi
    leaq user_rename_inner_name(%rip), %rsi
    movq $512, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    movq $80, %rax
    leaq user_rename_directory_root(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $80, %rax
    leaq user_parent_path(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $79, %rax
    movabsq $0x7fffff80, %rdi
    movq $64, %rsi
    syscall
    cmpq $6, %rax
    jne 1f
    cmpl $0x7369642f, 0x7fffff80
    jne 1f
    cmpw $0x006b, 0x7fffff84
    jne 1f
    movq $80, %rax
    leaq user_disk_absolute_path(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $82, %rax
    leaq user_rename_directory_root(%rip), %rdi
    leaq user_rename_directory_path(%rip), %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movq $84, %rax
    leaq user_rename_directory_name(%rip), %rdi
    syscall
    testq %rax, %rax
    jne 1f
    movq $258, %rax
    movq %rbx, %rdi
    leaq user_rename_inner_name(%rip), %rsi
    movq $0777, %rdx
    syscall
    cmpq $-2, %rax
    jne 1f
    movq $217, %rax
    movq %rbx, %rdi
    movabsq $0x7ffffc00, %rsi
    movq $64, %rdx
    syscall
    cmpq $48, %rax
    jne 1f
    movq $3, %rax
    movq %rbx, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $87, %rax
    leaq user_deleted_file_path(%rip), %rdi
    syscall
    cmpq $-2, %rax
    jne 1f

    leaq user_directory_growth_names(%rip), %r14
    movq $12, %r15
5:
    movq $257, %rax
    movq $-100, %rdi
    movq %r14, %rsi
    movq $64, %rdx
    movq $0644, %r10
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %rbx
    movq $3, %rax
    movq %rbx, %rdi
    syscall
    testq %rax, %rax
    jne 1f
    addq $8, %r14
    decq %r15
    jnz 5b

    movq $5, %rax
    movq %r12, %rdi
    movabsq $0x50000000, %rsi
    syscall
    cmpq $-14, %rax
    jne 1f

    movq $217, %rax
    movq %r12, %rdi
    movabsq $0x7ffffc00, %rsi
    movq $512, %rdx
    syscall
    cmpq $-20, %rax
    jne 1f

    movq $257, %rax
    movq $-100, %rdi
    leaq user_disk_path(%rip), %rsi
    movq $65536, %rdx
    xorq %r10, %r10
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %r14

    movq $5, %rax
    movq %r14, %rdi
    movq %r13, %rsi
    syscall
    testq %rax, %rax
    jne 1f
    movl 24(%r13), %eax
    andl $0170000, %eax
    cmpl $0040000, %eax
    jne 1f

    movq $257, %rax
    movq %r14, %rdi
    leaq user_file_path(%rip), %rsi
    xorq %rdx, %rdx
    syscall
    cmpq $3, %rax
    jb 1f
    movq %rax, %r11
    movq $3, %rax
    movq %r11, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movabsq $0x7ffffc00, %r15
    movq $217, %rax
    movq %r14, %rdi
    movq %r15, %rsi
    movq $512, %rdx
    syscall
    cmpq $496, %rax
    jne 1f
    cmpw $24, 16(%r15)
    jne 1f
    cmpb $4, 18(%r15)
    jne 1f
    cmpb $'.', 19(%r15)
    jne 1f
    cmpw $24, 40(%r15)
    jne 1f
    cmpb $'.', 43(%r15)
    jne 1f
    cmpb $'.', 44(%r15)
    jne 1f
    cmpb $8, 66(%r15)
    jne 1f
    movabsq $0x5458542e52455355, %rax
    cmpq %rax, 67(%r15)
    jne 1f

    movq $217, %rax
    movq %r14, %rdi
    movq %r15, %rsi
    movq $512, %rdx
    syscall
    cmpq $32, %rax
    jne 1f
    cmpb $8, 18(%r15)
    jne 1f
    movabsq $0x00504d542e313146, %rax
    cmpq %rax, 19(%r15)
    jne 1f

    movq $217, %rax
    movq %r14, %rdi
    movq %r15, %rsi
    movq $512, %rdx
    syscall
    testq %rax, %rax
    jne 1f

    movq $8, %rax
    movq %r14, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    syscall
    testq %rax, %rax
    jne 1f
    movq $217, %rax
    movq %r14, %rdi
    movq %r15, %rsi
    movq $24, %rdx
    syscall
    cmpq $24, %rax
    jne 1f

    movq $3, %rax
    movq %r14, %rdi
    syscall
    testq %rax, %rax
    jne 1f

    movq $257, %rax
    movq $-100, %rdi
    movabsq $0x50000000, %rsi
    xorq %rdx, %rdx
    syscall
    cmpq $-14, %rax
    jne 1f
    movq $257, %rax
    movq $-100, %rdi
    leaq user_missing_path(%rip), %rsi
    xorq %rdx, %rdx
    syscall
    cmpq $-2, %rax
    jne 1f

    movq $1, %rax
    movq $1, %rdi
    leaq user_file_pass(%rip), %rsi
    movq $(user_file_pass_end - user_file_pass), %rdx
    syscall
    cmpq $(user_file_pass_end - user_file_pass), %rax
    jne 1f
    xorq %rdi, %rdi
    jmp 2f
1:
    movq $1, %rdi
2:
    movq $60, %rax
    syscall
    ud2

user_file_path:
    .asciz "USER.TXT"
user_disk_path:
    .asciz "."
user_disk_absolute_path:
    .asciz "/disk/DOCS"
user_created_file_path:
    .asciz "/disk/CREATED.TXT"
user_truncate_file_path:
    .asciz "/disk/TRUNC.TMP"
user_moved_file_path:
    .asciz "/disk/DOCS/MOVED.TMP"
user_moved_file_name:
    .asciz "MOVED.TMP"
user_renamed_file_name:
    .asciz "RENAMED.TMP"
user_renamed_file_path:
    .asciz "/disk/DOCS/RENAMED.TMP"
user_rename_source:
    .asciz "SOURCE.TMP"
user_rename_target:
    .asciz "TARGET.TMP"
user_rename_directory_name:
    .asciz "RNDIR"
user_rename_inner_name:
    .asciz "INNER"
user_rename_directory_path:
    .asciz "/disk/DOCS/RNDIR"
user_rename_directory_root:
    .asciz "/disk/RNDIR"
user_rename_cycle_path:
    .asciz "/disk/DOCS/RNDIR/LOOP"
user_subdirectory_path:
    .asciz "SUB"
user_reused_directory_path:
    .asciz "AGAIN"
user_deleted_file_path:
    .asciz "DELETE.TMP"
user_reused_file_path:
    .asciz "REUSE.TMP"
user_parent_path:
    .asciz ".."
user_directory_growth_names:
    .ascii "F00.TMP\0F01.TMP\0F02.TMP\0F03.TMP\0"
    .ascii "F04.TMP\0F05.TMP\0F06.TMP\0F07.TMP\0"
    .ascii "F08.TMP\0F09.TMP\0F10.TMP\0F11.TMP\0"
user_missing_path:
    .asciz "MISSING.TXT"
user_file_data:
    .ascii "ring3-file-data"
user_file_data_end:
user_file_pass:
    .ascii "OPOS: ring-3 file syscalls passed\n"
user_file_pass_end:
user_file_program_end:

.section .note.GNU-stack,"",@progbits
