/* OPOS SDK C library: non-local jumps (x86-64; no signal mask). */
#ifndef OPOS_SETJMP_H
#define OPOS_SETJMP_H

typedef unsigned long jmp_buf[8];

int setjmp(jmp_buf env) __attribute__((returns_twice));
__attribute__((noreturn)) void longjmp(jmp_buf env, int value);

#endif
