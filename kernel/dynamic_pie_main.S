.section .text
.global _start
.type _start, @function
_start:
    call opos_shared_rodata_probe@PLT
    cmpq $61, %rax
    jne 1f
    call opos_shared_state@PLT
    cmpq $2, %rax
    jne 1f
    movq $1, %rax
    movq $1, %rdi
    leaq pie_message(%rip), %rsi
    movq $(pie_message_end - pie_message), %rdx
    syscall
    cmpq $(pie_message_end - pie_message), %rax
    jne 1f

    subq $32, %rsp
    leaq dynamic_test_path(%rip), %rax
    movq %rax, 0(%rsp)
    leaq dynamic_test_argument(%rip), %rax
    movq %rax, 8(%rsp)
    movq $0, 16(%rsp)
    movq $0, 24(%rsp)
    movq $59, %rax
    leaq dynamic_test_path(%rip), %rdi
    leaq 0(%rsp), %rsi
    leaq 24(%rsp), %rdx
    syscall
1:
    movq $1, %rdi
    movq $231, %rax
    syscall
    ud2

.global opos_main_export
.type opos_main_export, @function
opos_main_export:
    movq $73, %rax
    ret

.global opos_preempt_value
.type opos_preempt_value, @function
opos_preempt_value:
    movq $29, %rax
    ret

.section .rodata
pie_message:
    .ascii "OPOS: PIE main loaded at 0x04000000\n"
pie_message_end:
dynamic_test_path:
    .asciz "/bin/dynamic-test"
dynamic_test_argument:
    .asciz "dyn"

.section .data
.balign 8
pie_data_sentinel:
    .quad 1

.section .note.GNU-stack,"",@progbits
