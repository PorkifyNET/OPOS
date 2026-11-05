/*
 * Program entry for OPOS applications: calls
 *     int main(int argc, char **argv, char **envp)
 * and exits with its return value. The kernel starts a program with the
 * Linux x86-64 initial stack: argc, argv[], NULL, envp[], NULL, auxv.
 */
    .section .text
    .global _start
    .type _start, @function
_start:
    xorl %ebp, %ebp
    movq (%rsp), %rdi           /* argc */
    leaq 8(%rsp), %rsi          /* argv */
    leaq 16(%rsp,%rdi,8), %rdx  /* envp = argv + argc + 1 */
    andq $-16, %rsp
    call main
    movl %eax, %edi
    movl $231, %eax             /* exit_group */
    syscall
    hlt                         /* not reached */
    .size _start, . - _start

/*
 * ABI note: marks the file as an OPOS application built for ABI version 1
 * (ELF note name "OPOS", type 1, a 4-byte version). The kernel does not
 * need it yet; it lets later loaders tell OPOS programs from Linux ones.
 */
    .section .note.opos, "a", @note
    .balign 4
    .long 5                     /* name size, including the zero */
    .long 4                     /* descriptor size */
    .long 1                     /* type: ABI version */
    .asciz "OPOS"
    .balign 4
    .long 1                     /* OPOS ABI version */
    .balign 4

    .section .note.GNU-stack, "", @progbits
