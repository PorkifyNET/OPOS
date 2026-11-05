.section .text
.type opos_base_init, @function
opos_base_init:
    cmpq $0, opos_base_lifecycle_state(%rip)
    jne 1f
    movq $1, opos_base_lifecycle_state(%rip)
1:
    ret

.type opos_base_fini, @function
opos_base_fini:
    cmpq $5, opos_base_lifecycle_state(%rip)
    jne 1f
    movq $6, opos_base_lifecycle_state(%rip)
1:
    ret

.global opos_base_add
.type opos_base_add, @function
opos_base_add:
    leaq 7(%rdi), %rax
    ret

.type opos_ifunc_impl, @function
opos_ifunc_impl:
    movq $77, %rax
    ret

.type opos_ifunc_resolver, @function
opos_ifunc_resolver:
    leaq opos_ifunc_impl(%rip), %rax
    ret

.global opos_ifunc_api
.type opos_ifunc_api, @gnu_indirect_function
.set opos_ifunc_api, opos_ifunc_resolver

.type opos_local_ifunc, @gnu_indirect_function
.set opos_local_ifunc, opos_ifunc_resolver

.global opos_base_ifunc_probe
.type opos_base_ifunc_probe, @function
opos_base_ifunc_probe:
    subq $8, %rsp
    call opos_local_ifunc@PLT
    addq $8, %rsp
    ret

.global opos_base_transition
.type opos_base_transition, @function
opos_base_transition:
    xorq %rax, %rax
    cmpq %rdi, opos_base_lifecycle_state(%rip)
    jne 1f
    movq %rsi, opos_base_lifecycle_state(%rip)
    incq %rax
1:
    ret

.global opos_base_state
.type opos_base_state, @function
opos_base_state:
    movq opos_base_lifecycle_state(%rip), %rax
    ret

.weak opos_preempt_value
.type opos_preempt_value, @function
opos_preempt_value:
    movq $11, %rax
    ret

.global opos_versioned_v1
.type opos_versioned_v1, @function
opos_versioned_v1:
    movq $41, %rax
    ret
.symver opos_versioned_v1, opos_versioned_api@OPOS_1.0

.global opos_versioned_v2
.type opos_versioned_v2, @function
opos_versioned_v2:
    movq $42, %rax
    ret
.symver opos_versioned_v2, opos_versioned_api@@OPOS_2.0

.global opos_relr_target
.type opos_relr_target, @function
opos_relr_target:
    ret

.global opos_base_relr_probe
.type opos_base_relr_probe, @function
opos_base_relr_probe:
    leaq opos_relr_target(%rip), %rax
    leaq opos_relr_slots(%rip), %r11
    cmpq %rax, 0(%r11)
    jne 1f
    cmpq %rax, 8(%r11)
    jne 1f
    cmpq %rax, 16(%r11)
    jne 1f
    cmpq %rax, 24(%r11)
    jne 1f
    movq $1, %rax
    ret
1:
    xorq %rax, %rax
    ret

.global opos_base_tls_probe
.type opos_base_tls_probe, @function
opos_base_tls_probe:
    movq opos_base_tls_value@GOTTPOFF(%rip), %r11
    movq %fs:(%r11), %rax
    cmpq $17, %rax
    jne 1f
    movq opos_base_tls_zero@GOTTPOFF(%rip), %r11
    cmpq $0, %fs:(%r11)
    jne 1f
    movq $7, %fs:(%r11)
    movq $24, %rax
    ret
1:
    xorq %rax, %rax
    ret

.section .init_array,"aw",@init_array
    .quad opos_base_init

.section .fini_array,"aw",@fini_array
    .quad opos_base_fini

.section .data
.balign 8
opos_base_lifecycle_state:
    .quad 0

.section .tdata,"awT",@progbits
.balign 8
.global opos_base_tls_value
.type opos_base_tls_value, @object
opos_base_tls_value:
    .quad 17

.section .tbss,"awT",@nobits
.balign 8
.global opos_base_tls_zero
.type opos_base_tls_zero, @object
opos_base_tls_zero:
    .zero 8

.section .note.GNU-stack,"",@progbits
