.section .text
.global opos_shared_add
.type opos_shared_add, @function
opos_shared_add:
    subq $8, %rsp
    call opos_base_add@PLT
    addq $8, %rsp
    ret

.global opos_shared_rodata_probe
.type opos_shared_rodata_probe, @function
opos_shared_rodata_probe:
    movq opos_consumer_rodata(%rip), %rax
    ret

.global opos_shared_transition
.type opos_shared_transition, @function
opos_shared_transition:
    jmp opos_base_transition@PLT

.global opos_shared_state
.type opos_shared_state, @function
opos_shared_state:
    jmp opos_base_state@PLT

.global opos_shared_main_export
.type opos_shared_main_export, @function
opos_shared_main_export:
    subq $8, %rsp
    call opos_main_export@PLT
    addq $8, %rsp
    ret

.global opos_shared_preempt
.type opos_shared_preempt, @function
opos_shared_preempt:
    subq $8, %rsp
    call opos_preempt_value@PLT
    addq $8, %rsp
    ret

.weak opos_optional_value
.global opos_shared_optional_address
.type opos_shared_optional_address, @function
opos_shared_optional_address:
    movq opos_optional_value@GOTPCREL(%rip), %rax
    ret

.global opos_shared_relr_probe
.type opos_shared_relr_probe, @function
opos_shared_relr_probe:
    jmp opos_base_relr_probe@PLT

.global opos_shared_ifunc_probe
.type opos_shared_ifunc_probe, @function
opos_shared_ifunc_probe:
    subq $8, %rsp
    call opos_ifunc_api@PLT
    cmpq $77, %rax
    jne 1f
    call opos_base_ifunc_probe@PLT
    addq $8, %rsp
    cmpq $77, %rax
    jne 2f
    movq $154, %rax
    ret
1:
    addq $8, %rsp
2:
    xorq %rax, %rax
    ret

.global opos_shared_tls_probe
.type opos_shared_tls_probe, @function
opos_shared_tls_probe:
    movq opos_consumer_tls_value@GOTTPOFF(%rip), %r11
    movq %fs:(%r11), %rax
    cmpq $23, %rax
    jne 1f
    movq opos_base_tls_value@GOTTPOFF(%rip), %r11
    cmpq $17, %fs:(%r11)
    jne 1f
    subq $8, %rsp
    call opos_base_tls_probe@PLT
    addq $8, %rsp
    cmpq $24, %rax
    jne 1f
    movq opos_consumer_tls_zero@GOTTPOFF(%rip), %r11
    cmpq $0, %fs:(%r11)
    jne 1f
    movq $5, %fs:(%r11)
    movq $52, %rax
    ret
1:
    xorq %rax, %rax
    ret

.symver opos_versioned_legacy, opos_versioned_api@OPOS_1.0
.global opos_shared_version_probe
.type opos_shared_version_probe, @function
opos_shared_version_probe:
    subq $8, %rsp
    call opos_versioned_legacy@PLT
    cmpq $41, %rax
    jne 1f
    call opos_versioned_api@PLT
    addq $8, %rsp
    cmpq $42, %rax
    jne 2f
    movq $83, %rax
    ret
1:
    addq $8, %rsp
2:
    xorq %rax, %rax
    ret

.type opos_shared_init, @function
opos_shared_init:
    subq $8, %rsp
    movq $1, %rdi
    movq $2, %rsi
    call opos_base_transition@PLT
    addq $8, %rsp
    ret

.type opos_shared_fini, @function
opos_shared_fini:
    subq $8, %rsp
    movq $4, %rdi
    movq $5, %rsi
    call opos_base_transition@PLT
    addq $8, %rsp
    ret

.section .init_array,"aw",@init_array
    .quad opos_shared_init

.section .fini_array,"aw",@fini_array
    .quad opos_shared_fini

.section .data
.balign 8
.global opos_copy_source
.type opos_copy_source, @object
.size opos_copy_source, 16
opos_copy_source:
    .quad 0x1122334455667788
    .quad 0x8877665544332211

.section .tdata,"awT",@progbits
.balign 8
.global opos_consumer_tls_value
.hidden opos_consumer_tls_value
.type opos_consumer_tls_value, @object
opos_consumer_tls_value:
    .quad 23

.section .tbss,"awT",@nobits
.balign 8
.global opos_consumer_tls_zero
.hidden opos_consumer_tls_zero
.type opos_consumer_tls_zero, @object
opos_consumer_tls_zero:
    .zero 8

.section .rodata
.balign 8
opos_consumer_rodata:
    .quad 61

.section .note.GNU-stack,"",@progbits
