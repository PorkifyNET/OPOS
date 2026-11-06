/*
 * Program entry for OPOS applications: calls
 *     int main(int argc, char **argv, char **envp)
 * and exits with its return value. The kernel starts a program with the
 * Linux x86-64 initial stack: argc, argv[], NULL, envp[], NULL, auxv.
 *
 * That stack is a single 4 KiB page, so main runs on a larger one taken
 * from mmap, with an inaccessible guard page below it: running off the
 * end stops the program instead of overwriting other memory. If the
 * allocation fails, main runs on the kernel's page.
 */
#define STACK_SIZE (128 * 1024)
#define GUARD_SIZE 4096

    .section .text
    .global _start
    .type _start, @function
_start:
    xorl %ebp, %ebp
    movq %rsp, %r12             /* argc, argv, envp live here */

    movl $9, %eax               /* mmap */
    xorl %edi, %edi
    movl $(STACK_SIZE + GUARD_SIZE), %esi
    movl $3, %edx               /* read, write */
    movl $0x22, %r10d           /* private, anonymous */
    movq $-1, %r8
    xorl %r9d, %r9d
    syscall
    cmpq $-4096, %rax
    ja 1f                       /* failed: keep the kernel's stack */
    movq %rax, %r13
    movl $10, %eax              /* mprotect the guard page to PROT_NONE */
    movq %r13, %rdi
    movl $GUARD_SIZE, %esi
    xorl %edx, %edx
    syscall
    leaq (STACK_SIZE + GUARD_SIZE)(%r13), %rsp
1:
    movq (%r12), %rdi           /* argc */
    leaq 8(%r12), %rsi          /* argv */
    leaq 16(%r12,%rdi,8), %rdx  /* envp = argv + argc + 1 */
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
