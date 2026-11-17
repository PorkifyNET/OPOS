/* OPOS SDK C library: setjmp/longjmp (x86-64 System V) and the SSE
   rounding mode (see include/setjmp.h and include/fenv.h). */
#include <fenv.h>
#include <setjmp.h>

/* jmp_buf: rbx, rbp, r12, r13, r14, r15, rsp (after return), rip. */
__asm__(
    ".text\n"
    ".globl setjmp\n"
    ".type setjmp,@function\n"
    "setjmp:\n"
    "    movq %rbx, 0(%rdi)\n"
    "    movq %rbp, 8(%rdi)\n"
    "    movq %r12, 16(%rdi)\n"
    "    movq %r13, 24(%rdi)\n"
    "    movq %r14, 32(%rdi)\n"
    "    movq %r15, 40(%rdi)\n"
    "    leaq 8(%rsp), %rdx\n"
    "    movq %rdx, 48(%rdi)\n"
    "    movq (%rsp), %rdx\n"
    "    movq %rdx, 56(%rdi)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".size setjmp, .-setjmp\n"
    ".globl longjmp\n"
    ".type longjmp,@function\n"
    "longjmp:\n"
    "    movl %esi, %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    incl %eax\n"
    "1:\n"
    "    movq 0(%rdi), %rbx\n"
    "    movq 8(%rdi), %rbp\n"
    "    movq 16(%rdi), %r12\n"
    "    movq 24(%rdi), %r13\n"
    "    movq 32(%rdi), %r14\n"
    "    movq 40(%rdi), %r15\n"
    "    movq 48(%rdi), %rsp\n"
    "    jmp *56(%rdi)\n"
    ".size longjmp, .-longjmp\n");

int fegetround(void) {
    unsigned int mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    return (int)(mxcsr & 0x6000);
}

int fesetround(int mode) {
    if (mode & ~0x6000) {
        return -1;
    }
    unsigned int mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    mxcsr = (mxcsr & ~0x6000u) | (unsigned int)mode;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    return 0;
}
