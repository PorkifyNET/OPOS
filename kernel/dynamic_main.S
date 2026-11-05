.section .text
.global _start
.type _start, @function
_start:
    movq %rdx, %r12
    cmpq $67, opos_main_rodata(%rip)
    jne 1f
    call opos_shared_state@PLT
    cmpq $3, %rax
    jne 1f
    movq $35, %rdi
    call opos_shared_add@PLT
    cmpq $42, %rax
    jne 1f
    call opos_shared_rodata_probe@PLT
    cmpq $61, %rax
    jne 1f
    call opos_shared_main_export@PLT
    cmpq $73, %rax
    jne 1f
    call opos_shared_preempt@PLT
    cmpq $29, %rax
    jne 1f
    call opos_shared_optional_address@PLT
    testq %rax, %rax
    jne 1f
    movq %fs:opos_main_tls_value@TPOFF, %rax
    cmpq $31, %rax
    jne 1f
    cmpq $0, %fs:opos_main_tls_zero@TPOFF
    jne 1f
    movq $9, %fs:opos_main_tls_zero@TPOFF
    call opos_shared_tls_probe@PLT
    cmpq $52, %rax
    jne 1f
    call opos_shared_dynamic_tls_probe@PLT
    cmpq $54, %rax
    jne 1f
    call opos_shared_tlsdesc_probe@PLT
    cmpq $54, %rax
    jne 1f
    call opos_shared_relr_probe@PLT
    cmpq $1, %rax
    jne 1f
    call opos_shared_ifunc_probe@PLT
    cmpq $154, %rax
    jne 1f
    movabsq $0x1122334455667788, %r11
    cmpq %r11, opos_copy_source(%rip)
    jne 1f
    movabsq $0x8877665544332211, %r11
    cmpq %r11, opos_copy_source+8(%rip)
    jne 1f
    call opos_shared_version_probe@PLT
    cmpq $83, %rax
    jne 1f
    cmpq $9, %fs:opos_main_tls_zero@TPOFF
    jne 1f
    testq %r12, %r12
    je 1f
    call *%r12
    call opos_shared_state@PLT
    cmpq $6, %rax
    jne 1f
    subq $24, %rsp
    leaq musl_test_path(%rip), %rdi
    movq %rdi, 0(%rsp)
    movq $0, 8(%rsp)
    movq $0, 16(%rsp)
    movq %rsp, %rsi
    leaq 16(%rsp), %rdx
    movq $59, %rax
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

.global opos_versioned_api
.type opos_versioned_api, @function
opos_versioned_api:
    movq $99, %rax
    ret

.type opos_main_init, @function
opos_main_init:
    subq $8, %rsp
    movq $2, %rdi
    movq $3, %rsi
    call opos_shared_transition@PLT
    addq $8, %rsp
    ret

.type opos_main_fini, @function
opos_main_fini:
    subq $8, %rsp
    movq $3, %rdi
    movq $4, %rsi
    call opos_shared_transition@PLT
    addq $8, %rsp
    ret

.section .init_array,"aw",@init_array
    .quad opos_main_init

.section .fini_array,"aw",@fini_array
    .quad opos_main_fini

.section .tdata,"awT",@progbits
.balign 8
.global opos_main_tls_value
.type opos_main_tls_value, @object
opos_main_tls_value:
    .quad 31

.section .tbss,"awT",@nobits
.balign 8
.global opos_main_tls_zero
.type opos_main_tls_zero, @object
opos_main_tls_zero:
    .zero 8

.section .data
.balign 8
data_sentinel:
    .quad 1

.section .rodata
.balign 8
opos_main_rodata:
    .quad 67
musl_test_path:
    .asciz "/bin/musl-test"

.section .note.GNU-stack,"",@progbits
